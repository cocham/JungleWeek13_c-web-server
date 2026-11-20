#include "tcp.h"
#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { LISTEN_BACKLOG = 64, ACCEPT_WAIT_MS = 250, OVERLOAD_WRITE_MS = 50,
       WORKER_COUNT = 4, QUEUE_CAPACITY = 16 };

struct job {
    int client_fd;
    int64_t read_deadline;
};

struct pool {
    pthread_mutex_t mutex;
    pthread_cond_t available;
    struct job queue[QUEUE_CAPACITY];
    size_t head;
    size_t count;
    bool closing;
    const char *name;
    connection_handler handler;
    const void *context;
};

// Only the accept thread receives shutdown signals.
static volatile sig_atomic_t stopping;

static void request_stop(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

bool parse_port(const char *text, uint16_t *port)
{
    if (*text == '\0' || text[strspn(text, "0123456789")] != '\0') {
        return false;
    }
    unsigned long value = strtoul(text, NULL, 10);
    if (value == 0 || value > UINT16_MAX) {
        return false;
    }
    *port = (uint16_t)value;
    return true;
}

int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return -1;
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int64_t deadline_after(int milliseconds)
{
    int64_t now = monotonic_ms();
    return now < 0 ? -1 : now + milliseconds;
}

int wait_for_io(int fd, short events, int64_t deadline)
{
    struct pollfd socket = {.fd = fd, .events = events};
    if (deadline < 0) {
        errno = EIO;
        return -1;
    }
    for (;;) {
        int64_t now = monotonic_ms();
        if (now < 0) {
            return -1;
        }
        if (now >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
        int ready = poll(&socket, 1, (int)(deadline - now));
        if (ready > 0) {
            return 0;
        }
        if (ready < 0 && errno != EINTR) {
            return -1;
        }
    }
}

static bool retryable_io_error(void)
{
    return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
}

ssize_t receive_bytes(int fd, char *buffer, size_t capacity, int64_t deadline)
{
    while (wait_for_io(fd, POLLIN, deadline) == 0) {
        ssize_t received = recv(fd, buffer, capacity, 0);
        if (received >= 0 || !retryable_io_error()) {
            return received;
        }
    }
    return -1;
}

int send_all(int fd, const char *bytes, size_t length, int64_t deadline)
{
    while (length > 0) {
        if (wait_for_io(fd, POLLOUT, deadline) < 0) {
            return -1;
        }
        ssize_t sent = send(fd, bytes, length, 0);
        if (sent < 0 && retryable_io_error()) {
            continue;
        }
        if (sent <= 0) {
            return -1;
        }
        bytes += sent;
        length -= (size_t)sent;
    }
    return 0;
}

static int open_listener(uint32_t bind_address, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int reuse = 1;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = bind_address
    };
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse) < 0 ||
        set_nonblocking(fd) < 0 ||
        bind(fd, (const struct sockaddr *)&address, sizeof address) < 0 ||
        listen(fd, LISTEN_BACKLOG) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

static void close_client(const struct pool *pool, int client_fd)
{
    fprintf(stderr, "%s client_fd=%d closing\n", pool->name, client_fd);
    close(client_fd);
}

static void *worker_main(void *context)
{
    struct pool *pool = context;
    for (;;) {
        pthread_mutex_lock(&pool->mutex);
        while (!pool->closing && pool->count == 0) {
            pthread_cond_wait(&pool->available, &pool->mutex);
        }
        if (pool->closing) {
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }
        struct job job = pool->queue[pool->head];
        pool->head = (pool->head + 1) % QUEUE_CAPACITY;
        --pool->count;
        pthread_mutex_unlock(&pool->mutex);

        pool->handler(job.client_fd, job.read_deadline, pool->context);
        close_client(pool, job.client_fd);
    }
}

static bool enqueue(struct pool *pool, struct job job)
{
    pthread_mutex_lock(&pool->mutex);
    bool has_space = pool->count < QUEUE_CAPACITY;
    if (has_space) {
        size_t tail = (pool->head + pool->count) % QUEUE_CAPACITY;
        pool->queue[tail] = job;
        ++pool->count;
        pthread_cond_signal(&pool->available);
    }
    pthread_mutex_unlock(&pool->mutex);
    return has_space;
}

static void close_queue(struct pool *pool)
{
    pthread_mutex_lock(&pool->mutex);
    pool->closing = true;
    while (pool->count > 0) {
        close_client(pool, pool->queue[pool->head].client_fd);
        pool->head = (pool->head + 1) % QUEUE_CAPACITY;
        --pool->count;
    }
    pthread_cond_broadcast(&pool->available);
    pthread_mutex_unlock(&pool->mutex);
}

static int accept_clients(int listener, struct pool *pool)
{
    struct pollfd socket = {.fd = listener, .events = POLLIN};
    while (!stopping) {
        int ready = poll(&socket, 1, ACCEPT_WAIT_MS);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll listener");
            return -1;
        }
        if (ready == 0 || stopping) {
            continue;
        }
        int client_fd = accept(listener, NULL, NULL);
        if (client_fd < 0) {
            if (retryable_io_error() || errno == ECONNABORTED) {
                continue;
            }
            perror("accept");
            return -1;
        }
        struct job job = {.client_fd = client_fd, .read_deadline = deadline_after(CLIENT_READ_MS)};
        fprintf(stderr, "%s client_fd=%d accepted\n", pool->name, client_fd);
        if (stopping || set_nonblocking(client_fd) < 0) {
            close_client(pool, client_fd);
        } else if (!enqueue(pool, job)) {
            (void)send_error(client_fd, 503, NULL, deadline_after(OVERLOAD_WRITE_MS));
            close_client(pool, client_fd);
        }
    }
    return 0;
}

static int install_signals(sigset_t *handled)
{
    struct sigaction stop = {.sa_handler = request_stop};
    struct sigaction ignore = {.sa_handler = SIG_IGN};
    if (sigemptyset(handled) < 0 || sigaddset(handled, SIGINT) < 0 ||
        sigaddset(handled, SIGTERM) < 0 || sigemptyset(&stop.sa_mask) < 0 ||
        sigemptyset(&ignore.sa_mask) < 0 || sigaction(SIGINT, &stop, NULL) < 0 ||
        sigaction(SIGTERM, &stop, NULL) < 0 || sigaction(SIGPIPE, &ignore, NULL) < 0) {
        return -1;
    }
    return 0;
}

static int start_workers(struct pool *pool, pthread_t workers[], size_t *started,
                         const sigset_t *handled)
{
    int error = pthread_sigmask(SIG_BLOCK, handled, NULL);
    if (error != 0) {
        return error;
    }
    while (*started < WORKER_COUNT) {
        error = pthread_create(&workers[*started], NULL, worker_main, pool);
        if (error != 0) {
            break;
        }
        ++*started;
    }
    int unmask_error = pthread_sigmask(SIG_UNBLOCK, handled, NULL);
    return error != 0 ? error : unmask_error;
}

int serve_tcp(const char *name, uint32_t bind_address, uint16_t port,
              connection_handler handler, const void *context)
{
    sigset_t handled;
    if (install_signals(&handled) < 0) {
        perror("install signals");
        return -1;
    }
    int listener = open_listener(bind_address, port);
    if (listener < 0) {
        perror("open listener");
        return -1;
    }

    struct pool pool = {.name = name, .handler = handler, .context = context};
    int error = pthread_mutex_init(&pool.mutex, NULL);
    if (error != 0) {
        fprintf(stderr, "pthread_mutex_init: %s\n", strerror(error));
        close(listener);
        return -1;
    }
    error = pthread_cond_init(&pool.available, NULL);
    if (error != 0) {
        fprintf(stderr, "pthread_cond_init: %s\n", strerror(error));
        pthread_mutex_destroy(&pool.mutex);
        close(listener);
        return -1;
    }

    pthread_t workers[WORKER_COUNT];
    size_t started = 0;
    error = start_workers(&pool, workers, &started, &handled);
    int result = -1;
    if (error == 0) {
        fprintf(stderr, "%s listening on port %u with %d workers\n",
                name, (unsigned)port, WORKER_COUNT);
        result = accept_clients(listener, &pool);
    } else {
        fprintf(stderr, "start workers: %s\n", strerror(error));
    }
    close(listener);
    close_queue(&pool);
    for (size_t i = 0; i < started; ++i) {
        pthread_join(workers[i], NULL);
    }
    pthread_cond_destroy(&pool.available);
    pthread_mutex_destroy(&pool.mutex);
    return result;
}
