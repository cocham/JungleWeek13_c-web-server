// HTTP 파서·응답 생성·send_all()의 경계값 단위 테스트.
// socketpair()의 한쪽을 서버 FD로 쓰고, 다른 쪽에는 작성 스레드가 바이트를 조각내어 보낸다.
#include "http.h"
#include "tcp.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { GAP_MS = 20, QUICK_MS = 150, PATIENT_MS = 2000 };

static int checks;
static int failures;

static void check(bool ok, const char *text, const char *file, int line)
{
    ++checks;
    if (!ok) {
        ++failures;
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", file, line, text);
    }
}

#define CHECK(condition) check((condition), #condition, __FILE__, __LINE__)

static void check_status(int actual, int expected, const char *label, int line)
{
    ++checks;
    if (actual != expected) {
        ++failures;
        fprintf(stderr, "test_http.c:%d: %s: expected %d, got %d\n", line, label, expected, actual);
    }
}

#define CHECK_STATUS(actual, expected, label) check_status((actual), (expected), (label), __LINE__)

// ---------------------------------------------------------------------------
// socketpair와 작성 스레드

struct chunk {
    const char *data;
    size_t length;
};

#define TEXT(literal) {(literal), sizeof(literal) - 1}

struct feed {
    int fd;
    const struct chunk *chunks;
    size_t count;
    bool eof;
};

struct session {
    int server;
    int client;
    pthread_t feeder;
    struct feed feed;
};

static void sleep_ms(int milliseconds)
{
    struct timespec pause = {milliseconds / 1000, (long)(milliseconds % 1000) * 1000000L};
    while (nanosleep(&pause, &pause) < 0 && errno == EINTR) {
    }
}

static void write_fully(int fd, const char *data, size_t length)
{
    while (length > 0) {
        ssize_t written = write(fd, data, length);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        data += written;
        length -= (size_t)written;
    }
}

// 조각 사이에 GAP_MS를 두고 보내 서버가 조각마다 따로 recv()하게 만든다.
static void *feed_main(void *context)
{
    const struct feed *feed = context;
    for (size_t i = 0; i < feed->count; ++i) {
        if (i > 0) {
            sleep_ms(GAP_MS);
        }
        write_fully(feed->fd, feed->chunks[i].data, feed->chunks[i].length);
    }
    if (feed->eof) {
        shutdown(feed->fd, SHUT_WR);
    }
    return NULL;
}

static void open_pair(int fds[2])
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0 || set_nonblocking(fds[0]) < 0) {
        perror("socketpair");
        exit(2);
    }
}

static void open_session(struct session *session, const struct chunk *chunks, size_t count,
                         bool eof)
{
    int fds[2];
    open_pair(fds);
    session->server = fds[0];
    session->client = fds[1];
    session->feed = (struct feed){fds[1], chunks, count, eof};
    int error = pthread_create(&session->feeder, NULL, feed_main, &session->feed);
    if (error != 0) {
        fprintf(stderr, "pthread_create: %s\n", strerror(error));
        exit(2);
    }
}

// 서버 쪽을 닫은 뒤 클라이언트가 받은 바이트(서버가 보낸 것)를 모두 읽는다.
static size_t drain_client(int server, int client, char *reply, size_t capacity)
{
    close(server);
    size_t used = 0;
    while (used < capacity) {
        ssize_t count = read(client, reply + used, capacity - used);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        used += (size_t)count;
    }
    close(client);
    return used;
}

static size_t close_session(struct session *session, char *reply, size_t capacity)
{
    pthread_join(session->feeder, NULL);
    return drain_client(session->server, session->client, reply, capacity);
}

static struct request request;
static struct upstream_response upstream;
static char reply[HTTP_UPSTREAM_BODY_LIMIT];
static size_t reply_length;

static int run_request(const struct chunk *chunks, size_t count, bool eof, int timeout_ms)
{
    struct session session;
    open_session(&session, chunks, count, eof);
    int status = read_request(session.server, &request, deadline_after(timeout_ms));
    reply_length = close_session(&session, reply, sizeof reply);
    return status;
}

static enum upstream_result run_upstream(const struct chunk *chunks, size_t count, bool eof,
                                         int timeout_ms)
{
    struct session session;
    open_session(&session, chunks, count, eof);
    enum upstream_result result = read_upstream_response(session.server, &upstream,
                                                         deadline_after(timeout_ms));
    reply_length = close_session(&session, reply, sizeof reply);
    return result;
}

#define CHUNKS(...) (const struct chunk[]){__VA_ARGS__}
#define COUNT(...) (sizeof(CHUNKS(__VA_ARGS__)) / sizeof(struct chunk))
// 조각을 모두 보낸 뒤 EOF를 보낸다.
#define REQUEST_EOF(...) run_request(CHUNKS(__VA_ARGS__), COUNT(__VA_ARGS__), true, PATIENT_MS)
// 조각을 보낸 뒤 연결을 연 채로 둔다. 기한을 짧게 줘 408을 확인할 때 쓴다.
#define REQUEST_OPEN(...) run_request(CHUNKS(__VA_ARGS__), COUNT(__VA_ARGS__), false, QUICK_MS)
#define UPSTREAM_EOF(...) run_upstream(CHUNKS(__VA_ARGS__), COUNT(__VA_ARGS__), true, PATIENT_MS)
#define UPSTREAM_OPEN(...) run_upstream(CHUNKS(__VA_ARGS__), COUNT(__VA_ARGS__), false, QUICK_MS)

static bool same_bytes(struct http_body body, const char *data, size_t length)
{
    return body.length == length && memcmp(body.data, data, length) == 0;
}

#define BODY_IS(body, literal) same_bytes((body), (literal), sizeof(literal) - 1)

// ---------------------------------------------------------------------------
// #5 요청 본문과 Content-Length 경계

#define POST_HEAD(fields) "POST /api/echo HTTP/1.1\r\nHost: localhost\r\n" fields "\r\n"
// 헤더 끝처럼 보이는 CRLF CRLF, NUL, 상태 줄을 담은 본문.
#define TRICKY_BODY "a\0\r\n\r\nHTTP/1.1 200 OK\r\n"
_Static_assert(sizeof(TRICKY_BODY) - 1 == 23, "Content-Length below must match");

static void test_body_bytes(void)
{
    // 헤더와 함께 도착한 본문 바이트를 버리지 않는다.
    CHECK_STATUS(REQUEST_EOF(TEXT(POST_HEAD("Content-Length: 5\r\n") "hello")), 200,
                 "body prefetched with the head");
    CHECK(BODY_IS(request.body, "hello"));

    CHECK_STATUS(REQUEST_EOF(TEXT(POST_HEAD("Content-Length: 5\r\n") "he"), TEXT("llo")), 200,
                 "part of the body prefetched");
    CHECK(BODY_IS(request.body, "hello"));

    CHECK_STATUS(REQUEST_EOF(TEXT(POST_HEAD("Content-Length: 5\r\n")), TEXT("he"), TEXT("llo")),
                 200, "body after the head, in pieces");
    CHECK(BODY_IS(request.body, "hello"));

    // 빈 줄(CRLF CRLF) 자체가 두 번의 recv()로 나뉘어도 경계를 찾는다.
    CHECK_STATUS(REQUEST_EOF(TEXT("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r"),
                             TEXT("\nhel"), TEXT("lo")), 200, "empty line split across reads");
    CHECK(BODY_IS(request.body, "hello"));

    // 본문 안의 CRLF CRLF·NUL은 헤더 경계나 오류로 취급하지 않는다.
    CHECK_STATUS(REQUEST_EOF(TEXT(POST_HEAD("Content-Length: 23\r\n") TRICKY_BODY)), 200,
                 "binary body that looks like a header end");
    CHECK(BODY_IS(request.body, TRICKY_BODY));

    // 본문 상한 4096바이트는 받고, 4097은 413.
    static char exact[256 + HTTP_REQUEST_BODY_LIMIT];
    int head = snprintf(exact, 256, POST_HEAD("Content-Length: %d\r\n"), HTTP_REQUEST_BODY_LIMIT);
    memset(exact + head, 'x', HTTP_REQUEST_BODY_LIMIT);
    CHECK_STATUS(run_request(CHUNKS({exact, (size_t)head + HTTP_REQUEST_BODY_LIMIT}), 1, true,
                             PATIENT_MS), 200, "body exactly at the limit");
    CHECK(request.body.length == HTTP_REQUEST_BODY_LIMIT);

    CHECK_STATUS(REQUEST_EOF(TEXT("GET / HTTP/1.1\r\nHost: a\r\n\r\n")), 200, "GET without length");
    CHECK(request.body.length == 0);
}

static void test_content_length(void)
{
    // 과소 선언: 선언한 길이만 본문이다. 나머지는 이 연결에서 처리하지 않는다.
    CHECK_STATUS(REQUEST_EOF(TEXT(POST_HEAD("Content-Length: 3\r\n") "abcdef")), 200,
                 "Content-Length smaller than the bytes sent");
    CHECK(BODY_IS(request.body, "abc"));

    // 과대 선언: 클라이언트가 끝내면 400, 기다리기만 하면 기한에 408.
    CHECK_STATUS(REQUEST_EOF(TEXT(POST_HEAD("Content-Length: 10\r\n") "abc")), 400,
                 "EOF before the declared length");
    CHECK_STATUS(REQUEST_OPEN(TEXT(POST_HEAD("Content-Length: 10\r\n") "abc")), 408,
                 "silence before the declared length");

    static const struct {
        const char *fields;
        int status;
    } cases[] = {
        {"Content-Length: 0\r\n", 200},
        {"Content-Length: 007\r\n", 400},  // 7로 읽고 EOF까지 3바이트 모자람
        {"Content-Length: 4097\r\n", 413},
        {"Content-Length: 99999999999999999999999\r\n", 413},
        {"Content-Length: abc\r\n", 400},
        {"Content-Length: -1\r\n", 400},
        {"Content-Length: +5\r\n", 400},
        {"Content-Length: 0x10\r\n", 400},
        {"Content-Length: 5, 5\r\n", 400},
        {"Content-Length:\r\n", 400},
        {"Content-Length: 5\r\nContent-Length: 5\r\n", 400},
        // 지원하지 않는 전송 인코딩은 본문 경계를 추측하지 않고 거부한다.
        {"Transfer-Encoding: chunked\r\n", 501},
        {"Transfer-Encoding: identity\r\n", 501},
        {"Transfer-Encoding: chunked\r\nContent-Length: 5\r\n", 400},
        {"", 411},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        char head[256];
        int length = snprintf(head, sizeof head, POST_HEAD("%s") "abcd", cases[i].fields);
        CHECK_STATUS(run_request(CHUNKS({head, (size_t)length}), 1, true, PATIENT_MS),
                     cases[i].status, cases[i].fields[0] ? cases[i].fields : "(no length)");
    }

    CHECK_STATUS(REQUEST_EOF(TEXT("POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n")), 400,
                 "Transfer-Encoding in HTTP/1.0");
    CHECK_STATUS(REQUEST_EOF(TEXT("GET / HTTP/1.1\r\nHost: a\0b\r\n\r\n")), 400, "NUL in the head");
    CHECK_STATUS(run_request(NULL, 0, true, PATIENT_MS), 0, "EOF before any byte");
    CHECK_STATUS(REQUEST_EOF(TEXT("GET / HTTP/1.1\r\nHost:")), 400, "EOF inside the head");
    CHECK_STATUS(REQUEST_OPEN(TEXT("GET / HTTP/1.1\r\nHost: a\r\n")), 408, "silence inside the head");
}

static void test_expect(void)
{
    static const char interim[] = "HTTP/1.1 100 Continue\r\n\r\n";
    CHECK_STATUS(REQUEST_EOF(TEXT(POST_HEAD("Expect: 100-continue\r\nContent-Length: 5\r\n")),
                             TEXT("hello")), 200, "100-continue before the body");
    CHECK(reply_length == sizeof interim - 1 && memcmp(reply, interim, reply_length) == 0);
    CHECK(BODY_IS(request.body, "hello"));

    CHECK_STATUS(REQUEST_EOF(TEXT(POST_HEAD("Expect: 100-continue\r\nContent-Length: 5\r\n")
                                  "hello")), 200, "100-continue with the body already here");
    CHECK(reply_length == 0);

    CHECK_STATUS(REQUEST_EOF(TEXT(POST_HEAD("Expect: 100-continue\r\nContent-Length: 4097\r\n"))),
                 413, "100-continue for a body over the limit");
    CHECK(reply_length == 0);  // 413만 보내고 본문을 청하지 않는다.
}

// ---------------------------------------------------------------------------
// #6 send_all()의 부분 송신

struct reader {
    int fd;
    char *data;
    size_t capacity;
    size_t used;
};

// 조금씩, 천천히 읽어 송신 측 버퍼를 계속 가득 차게 만든다.
static void *slow_reader(void *context)
{
    struct reader *reader = context;
    while (reader->used < reader->capacity) {
        size_t want = reader->capacity - reader->used < 1024 ? reader->capacity - reader->used : 1024;
        ssize_t count = read(reader->fd, reader->data + reader->used, want);
        if (count <= 0) {
            break;
        }
        reader->used += (size_t)count;
        sleep_ms(1);
    }
    return NULL;
}

static void shrink_send_buffer(int fd)
{
    int size = 4096;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof size) < 0) {
        perror("SO_SNDBUF");
        exit(2);
    }
}

static void test_send_all(void)
{
    enum { SIZE = 256 * 1024 };
    static char data[SIZE];
    static char received[SIZE];
    for (size_t i = 0; i < SIZE; ++i) {
        data[i] = (char)(i * 31 + 7);
    }

    // 송신 버퍼가 수 KiB라 send()가 여러 번 부분 성공한다. 끝까지 이어 보내야 한다.
    int fds[2];
    open_pair(fds);
    shrink_send_buffer(fds[0]);
    struct reader reader = {fds[1], received, SIZE, 0};
    pthread_t thread;
    pthread_create(&thread, NULL, slow_reader, &reader);
    CHECK(send_all(fds[0], data, SIZE, deadline_after(10000)) == 0);
    close(fds[0]);
    pthread_join(thread, NULL);
    close(fds[1]);
    CHECK(reader.used == SIZE && memcmp(received, data, SIZE) == 0);

    // 아무도 읽지 않으면 기한에 -1, errno=ETIMEDOUT으로 끝난다.
    open_pair(fds);
    shrink_send_buffer(fds[0]);
    errno = 0;
    CHECK(send_all(fds[0], data, SIZE, deadline_after(QUICK_MS)) == -1 && errno == ETIMEDOUT);
    close(fds[0]);
    close(fds[1]);

    // 상대가 닫았으면 SIGPIPE 없이 -1, errno=EPIPE다.
    open_pair(fds);
    close(fds[1]);
    errno = 0;
    CHECK(send_all(fds[0], "x", 1, deadline_after(QUICK_MS)) == -1 && errno == EPIPE);
    close(fds[0]);
}

// ---------------------------------------------------------------------------
// #6 응답 형식

struct parsed {
    int status;
    char reason[64];
    long length;
    bool close;
    char type[128];
    char allow[64];
    const char *body;
    size_t body_length;
    int status_lines;
};

static void header_value(const char *line, const char *name, char *out, size_t capacity)
{
    size_t length = strlen(name);
    if (strncasecmp(line, name, length) == 0 && line[length] == ':') {
        const char *value = line + length + 1;
        value += strspn(value, " ");
        snprintf(out, capacity, "%s", value);
    }
}

static bool parse_reply(const char *data, size_t size, struct parsed *out)
{
    *out = (struct parsed){.length = -1};
    size_t end = 0;
    while (end + 4 <= size && memcmp(data + end, "\r\n\r\n", 4) != 0) {
        ++end;
    }
    if (end + 4 > size || end >= 1024) {
        return false;
    }
    char head[1024];
    memcpy(head, data, end);
    head[end] = '\0';
    out->body = data + end + 4;
    out->body_length = size - end - 4;
    for (size_t i = 0; i + 9 <= size; ++i) {
        out->status_lines += memcmp(data + i, "HTTP/1.1 ", 9) == 0;
    }
    char *line = strtok(head, "\r\n");
    if (line == NULL || sscanf(line, "HTTP/1.1 %d %63[^\r]", &out->status, out->reason) < 1) {
        return false;
    }
    char length[32] = "";
    char connection[32] = "";
    while ((line = strtok(NULL, "\r\n")) != NULL) {
        header_value(line, "Content-Length", length, sizeof length);
        header_value(line, "Connection", connection, sizeof connection);
        header_value(line, "Content-Type", out->type, sizeof out->type);
        header_value(line, "Allow", out->allow, sizeof out->allow);
    }
    out->length = length[0] ? strtol(length, NULL, 10) : -1;
    out->close = strcmp(connection, "close") == 0;
    return true;
}

static size_t capture_error(int status, const char *allow, bool head, bool with_request)
{
    int fds[2];
    open_pair(fds);
    request.head = head;
    int result = with_request
        ? send_error_for(fds[0], &request, status, allow, deadline_after(PATIENT_MS))
        : send_error(fds[0], status, allow, deadline_after(PATIENT_MS));
    CHECK(result == 0);
    return drain_client(fds[0], fds[1], reply, sizeof reply);
}

static void test_error_responses(void)
{
    static const int statuses[] = {400, 403, 404, 405, 408, 409, 411, 413, 414, 415, 417,
                                   431, 500, 501, 502, 503, 504, 505};
    for (size_t i = 0; i < sizeof statuses / sizeof statuses[0]; ++i) {
        int status = statuses[i];
        const char *allow = status == 405 ? "GET, HEAD" : NULL;
        struct parsed get;
        struct parsed head;
        char label[64];
        snprintf(label, sizeof label, "error %d", status);

        size_t size = capture_error(status, allow, false, true);
        CHECK(parse_reply(reply, size, &get));
        CHECK_STATUS(get.status, status, label);
        CHECK(get.reason[0] != '\0');
        CHECK(get.close);
        CHECK(strcmp(get.type, "text/plain; charset=utf-8") == 0);
        // Content-Length는 실제 본문 길이와 같고, 본문 뒤에 다른 응답이 붙지 않는다.
        CHECK(get.length > 0 && (size_t)get.length == get.body_length);
        CHECK(get.status_lines == 1);
        CHECK(get.body_length == strlen(get.reason) + 1 &&
              memcmp(get.body, get.reason, strlen(get.reason)) == 0);
        CHECK(allow == NULL ? get.allow[0] == '\0' : strcmp(get.allow, allow) == 0);

        // HEAD 요청에 대한 오류도 같은 헤더를 보내되 본문은 없다.
        size = capture_error(status, allow, true, true);
        CHECK(parse_reply(reply, size, &head));
        CHECK_STATUS(head.status, status, label);
        CHECK(head.length == get.length && head.body_length == 0);

        // 요청을 모르는 send_error()는 GET과 같은 응답을 보낸다.
        size = capture_error(status, allow, true, false);
        CHECK(parse_reply(reply, size, &head));
        CHECK(head.length == get.length && head.body_length == get.body_length);
    }
}

static void test_success_responses(void)
{
    static const char binary[] = "{\"a\":\"\0\r\n\"}";
    struct http_body body = {binary, sizeof binary - 1};
    for (int head = 0; head <= 1; ++head) {
        int fds[2];
        open_pair(fds);
        request.head = head;
        CHECK(send_response_for(fds[0], &request, 201, "application/json", body,
                                deadline_after(PATIENT_MS)) == 0);
        size_t size = drain_client(fds[0], fds[1], reply, sizeof reply);
        struct parsed parsed;
        CHECK(parse_reply(reply, size, &parsed));
        CHECK(parsed.status == 201 && parsed.close &&
              strcmp(parsed.type, "application/json") == 0);
        CHECK(parsed.length == (long)body.length);
        CHECK(head ? parsed.body_length == 0
                   : same_bytes((struct http_body){parsed.body, parsed.body_length},
                                binary, body.length));
    }
}

// ---------------------------------------------------------------------------
// 프록시가 기대하는 백엔드 응답 경계

static void test_upstream(void)
{
    CHECK(UPSTREAM_EOF(TEXT("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                            "Content-Length: 2\r\n\r\nok")) == UPSTREAM_OK);
    CHECK(upstream.status == 200 && BODY_IS(upstream.body, "ok") &&
          strcmp(upstream.content_type, "text/plain") == 0);

    CHECK(UPSTREAM_EOF(TEXT("HTTP/1.1 201 Created\r\nContent-Length: 5\r\n\r\nhe"),
                       TEXT("llo")) == UPSTREAM_OK);
    CHECK(upstream.status == 201 && BODY_IS(upstream.body, "hello") &&
          strcmp(upstream.content_type, "application/octet-stream") == 0);

    static const char *const invalid[] = {
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokk",        // 선언보다 많음
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nok",         // 선언보다 적고 EOF
        "HTTP/1.1 200 OK\r\n\r\nok",                              // 길이 없음
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\nok",
        "HTTP/1.1 200 OK\r\nContent-Length: 65537\r\n\r\n",
        "HTTP/1.1 100 Continue\r\n\r\n",
        "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 304 Not Modified\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok",
        "",
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        enum upstream_result result = run_upstream(CHUNKS({invalid[i], strlen(invalid[i])}), 1,
                                                   true, PATIENT_MS);
        if (result != UPSTREAM_INVALID) {
            ++failures;
            fprintf(stderr, "upstream case %zu: expected UPSTREAM_INVALID, got %d\n", i, result);
        }
        ++checks;
    }

    CHECK(UPSTREAM_OPEN(TEXT("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nok")) ==
          UPSTREAM_TIMEOUT);
    CHECK(UPSTREAM_OPEN(TEXT("HTTP/1.1 200 OK\r\n")) == UPSTREAM_TIMEOUT);
}

int main(void)
{
    // 서버 프로세스와 같이, 닫힌 소켓에 쓰면 SIGPIPE 대신 EPIPE를 받는다.
    signal(SIGPIPE, SIG_IGN);
    test_body_bytes();
    test_content_length();
    test_expect();
    test_send_all();
    test_error_responses();
    test_success_responses();
    test_upstream();
    printf("test_http: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
