#include "proxy.h"
#include "static.h"
#include "tcp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { CONNECT_MS = 500, UPSTREAM_WRITE_MS = 1000, UPSTREAM_READ_MS = 2000 };

static int connect_backend(int fd, const struct sockaddr_in *address)
{
    int64_t deadline = deadline_after(CONNECT_MS);
    if (set_nonblocking(fd) < 0) {
        return -1;
    }
    if (connect(fd, (const struct sockaddr *)address, sizeof *address) == 0) {
        return 0;
    }
    if (errno != EINPROGRESS && errno != EINTR) {
        return -1;
    }
    if (wait_for_io(fd, POLLOUT, deadline) < 0) {
        return -1;
    }
    int error;
    socklen_t length = sizeof error;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0) {
        return -1;
    }
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

static int send_upstream_request(int fd, const struct request *request,
                                 const struct sockaddr_in *address)
{
    char host[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &address->sin_addr, host, sizeof host) == NULL) {
        return -1;
    }
    const char *content_type = request->content_type;
    // Ask the backend for GET and drop the content on the way out: the response framing
    // stays the same and the backend needs no HEAD support (RFC 9110 9.3.2).
    const char *method = request->head ? "GET" : request->method;
    // The rebuilt head carries the client's request line and Content-Type, each within its limit.
    char header[HTTP_REQUEST_HEAD_LIMIT + 256];
    int length = snprintf(header, sizeof header,
        "%s %s HTTP/1.1\r\nHost: %s:%u\r\nVia: 1.1 guestbook\r\n"
        "Content-Length: %zu\r\n%s%s%sConnection: close\r\n\r\n",
        method, request->target, host, (unsigned)ntohs(address->sin_port),
        request->body.length, content_type == NULL ? "" : "Content-Type: ",
        content_type == NULL ? "" : content_type, content_type == NULL ? "" : "\r\n");
    if (length < 0 || (size_t)length >= sizeof header) {
        errno = EOVERFLOW;
        return -1;
    }
    int64_t deadline = deadline_after(UPSTREAM_WRITE_MS);
    if (send_all(fd, header, (size_t)length, deadline) < 0) {
        return -1;
    }
    return send_all(fd, request->body.data, request->body.length, deadline);
}

static int proxy_request(int client_fd, const struct request *request,
                         const struct sockaddr_in *address)
{
    int backend_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (backend_fd < 0) {
        return send_error_for(client_fd, request, 502, NULL, deadline_after(CLIENT_WRITE_MS));
    }
    fprintf(stderr, "proxy client_fd=%d backend_fd=%d opened\n", client_fd, backend_fd);
    struct upstream_response response;
    enum upstream_result result;
    if (connect_backend(backend_fd, address) < 0 ||
        send_upstream_request(backend_fd, request, address) < 0) {
        result = errno == ETIMEDOUT ? UPSTREAM_TIMEOUT : UPSTREAM_INVALID;
    } else {
        result = read_upstream_response(backend_fd, &response, deadline_after(UPSTREAM_READ_MS));
    }
    fprintf(stderr, "proxy client_fd=%d backend_fd=%d closing\n", client_fd, backend_fd);
    close(backend_fd);

    int64_t deadline = deadline_after(CLIENT_WRITE_MS);
    if (result != UPSTREAM_OK) {
        int status = result == UPSTREAM_TIMEOUT ? 504 : 502;
        return send_error_for(client_fd, request, status, NULL, deadline);
    }
    return send_response_for(client_fd, request, response.status, response.content_type,
                             response.body, deadline);
}

static const struct sockaddr_in *next_backend(struct backend_pool *pool)
{
    size_t index = atomic_load_explicit(&pool->next, memory_order_relaxed);
    // Only the cursor is mutable once the workers start.
    while (!atomic_compare_exchange_weak_explicit(&pool->next, &index,
                                                  (index + 1) % pool->count,
                                                  memory_order_relaxed, memory_order_relaxed)) {
    }
    return &pool->addresses[index];
}

int route_request(int client_fd, const struct request *request, const struct server_config *config)
{
    int64_t deadline = deadline_after(CLIENT_WRITE_MS);
    // RFC 9110 9.1: general-purpose servers MUST support GET and HEAD. read_request() has
    // already answered unimplemented methods with 501, so only GET, HEAD and POST reach here.
    bool is_get = request->head || strcmp(request->method, "GET") == 0;
    size_t prefix_length = strlen(config->route_prefix);
    if (request_path_length(request) >= prefix_length &&
        strncmp(request->target, config->route_prefix, prefix_length) == 0) {
        if (is_get && request->body.length != 0) {
            return send_error_for(client_fd, request, 400, NULL, deadline);
        }
        return proxy_request(client_fd, request, next_backend(config->backends));
    }
    if (!is_get) {
        return send_error_for(client_fd, request, 405, "GET, HEAD", deadline);
    }
    return serve_static(client_fd, config->static_root_fd, request, deadline);
}
