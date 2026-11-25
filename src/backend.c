#include "http.h"
#include "tcp.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum {
    MESSAGE_LIMIT = 240,
    MESSAGE_CAPACITY = 100,
    LIST_BUFFER_SIZE = sizeof "{\"messages\":[]}" + MESSAGE_CAPACITY * (2 * MESSAGE_LIMIT + 3)
};

_Static_assert((int)LIST_BUFFER_SIZE <= (int)HTTP_UPSTREAM_BODY_LIMIT,
               "The proxy must be able to buffer a full guestbook");

struct message {
    char bytes[MESSAGE_LIMIT];
    size_t length;
};

static struct {
    pthread_mutex_t mutex;
    struct message entries[MESSAGE_CAPACITY];
    size_t count;
} guestbook = {.mutex = PTHREAD_MUTEX_INITIALIZER};

static bool is_plain_utf8(const char *type)
{
    if (type == NULL) {
        return false;
    }
    const char *param = strchr(type, ';');
    size_t length = param == NULL ? strlen(type) : (size_t)(param - type);
    while (length > 0 && (type[length - 1] == ' ' || type[length - 1] == '\t')) {
        --length;
    }
    if (length != sizeof "text/plain" - 1 || strncasecmp(type, "text/plain", length) != 0) {
        return false;
    }
    if (param == NULL) {
        return true;
    }
    ++param;
    param += strspn(param, " \t");
    if (strncasecmp(param, "charset", sizeof "charset" - 1) != 0) {
        return false;
    }
    param += sizeof "charset" - 1;
    param += strspn(param, " \t");
    if (*param++ != '=') {
        return false;
    }
    param += strspn(param, " \t");
    return strcasecmp(param, "utf-8") == 0 || strcasecmp(param, "\"utf-8\"") == 0;
}

static bool valid_message(struct http_body body)
{
    const unsigned char *bytes = (const unsigned char *)body.data;
    bool has_text = false;
    for (size_t i = 0; i < body.length;) {
        uint32_t code = bytes[i++];
        if (code >= 0x80) {
            size_t extra;
            uint32_t minimum;
            if (code >= 0xc2 && code <= 0xdf) {
                extra = 1;
                minimum = 0x80;
                code &= 0x1f;
            } else if (code >= 0xe0 && code <= 0xef) {
                extra = 2;
                minimum = 0x800;
                code &= 0x0f;
            } else if (code >= 0xf0 && code <= 0xf4) {
                extra = 3;
                minimum = 0x10000;
                code &= 0x07;
            } else {
                return false;
            }
            if (body.length - i < extra) {
                return false;
            }
            for (size_t j = 0; j < extra; ++j) {
                unsigned char next = bytes[i++];
                if ((next & 0xc0) != 0x80) {
                    return false;
                }
                code = (code << 6) | (next & 0x3f);
            }
            if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
                return false;
            }
        }
        if (code < 0x20 || (code >= 0x7f && code <= 0x9f) ||
            code == 0x2028 || code == 0x2029) {
            return false;
        }
        has_text = has_text || code != ' ';
    }
    return has_text;
}

static char *write_json_string(char *out, struct http_body message)
{
    *out++ = '"';
    for (size_t i = 0; i < message.length; ++i) {
        char byte = message.data[i];
        if (byte == '"' || byte == '\\') {
            *out++ = '\\';
        }
        *out++ = byte;
    }
    *out++ = '"';
    return out;
}

static void send_json(int fd, int status, const char *begin, const char *end, int64_t deadline)
{
    (void)send_response(fd, status, "application/json; charset=utf-8",
                        (struct http_body){begin, (size_t)(end - begin)}, deadline);
}

static void send_message_list(int fd, int64_t deadline)
{
    const char prefix[] = "{\"messages\":[";
    char json[LIST_BUFFER_SIZE];
    memcpy(json, prefix, sizeof prefix - 1);
    char *out = json + sizeof prefix - 1;
    pthread_mutex_lock(&guestbook.mutex);
    for (size_t i = 0; i < guestbook.count; ++i) {
        const struct message *entry = &guestbook.entries[i];
        if (i > 0) {
            *out++ = ',';
        }
        out = write_json_string(out, (struct http_body){entry->bytes, entry->length});
    }
    pthread_mutex_unlock(&guestbook.mutex);
    *out++ = ']';
    *out++ = '}';
    send_json(fd, 200, json, out, deadline);
}

static bool store_message(struct http_body body)
{
    pthread_mutex_lock(&guestbook.mutex);
    bool available = guestbook.count < MESSAGE_CAPACITY;
    if (available) {
        struct message *entry = &guestbook.entries[guestbook.count++];
        memcpy(entry->bytes, body.data, body.length);
        entry->length = body.length;
    }
    pthread_mutex_unlock(&guestbook.mutex);
    return available;
}

static void register_message(int fd, const struct request *request, int64_t deadline)
{
    struct http_body body = request->body;
    if (!is_plain_utf8(request->content_type)) {
        (void)send_error(fd, 415, NULL, deadline);
        return;
    }
    if (body.length > MESSAGE_LIMIT) {
        (void)send_error(fd, 413, NULL, deadline);
        return;
    }
    if (!valid_message(body)) {
        (void)send_error(fd, 400, NULL, deadline);
        return;
    }
    if (!store_message(body)) {
        (void)send_error(fd, 409, NULL, deadline);
        return;
    }
    char json[sizeof "{\"message\":}" + 2 * MESSAGE_LIMIT + 2] = "{\"message\":";
    char *out = write_json_string(json + sizeof "{\"message\":" - 1, body);
    *out++ = '}';
    send_json(fd, 201, json, out, deadline);
}

static void handle_messages(int client_fd, const struct request *request, int64_t deadline)
{
    const char path[] = "/api/messages";
    if (request_path_length(request) != sizeof path - 1 ||
        strncmp(request->target, path, sizeof path - 1) != 0) {
        (void)send_error(client_fd, 404, NULL, deadline);
    } else if (strcmp(request->method, "GET") == 0) {
        send_message_list(client_fd, deadline);
    } else if (strcmp(request->method, "POST") == 0) {
        register_message(client_fd, request, deadline);
    } else {
        (void)send_error(client_fd, 405, "GET, POST", deadline);
    }
}

// 받은 본문과 Content-Type을 그대로 돌려준다. 앞단이 본문 바이트를 바꾸지 않고
// Content-Length 그대로 중계하는지 확인하는 검증용 경로이며 방명록 저장소는 쓰지 않는다.
static void send_echo(int client_fd, const struct request *request, int64_t deadline)
{
    const char *type = request->content_type;
    (void)send_response(client_fd, 200,
                        type == NULL || *type == '\0' ? "application/octet-stream" : type,
                        request->body, deadline);
}

static bool is_echo(const struct request *request)
{
    const char path[] = "/api/echo";
    return request_path_length(request) == sizeof path - 1 &&
           strncmp(request->target, path, sizeof path - 1) == 0;
}

static void handle_backend_connection(int client_fd, int64_t read_deadline, const void *context)
{
    (void)context;
    struct request request;
    int status = read_request(client_fd, &request, read_deadline);
    int64_t deadline = deadline_after(CLIENT_WRITE_MS);
    if (status == 200 && is_echo(&request)) {
        send_echo(client_fd, &request, deadline);
    } else if (status == 200) {
        handle_messages(client_fd, &request, deadline);
    } else if (status != 0) {
        (void)send_error(client_fd, status, NULL, deadline);
    }
}

int main(int argc, char **argv)
{
    uint16_t port = 8001;
    struct in_addr bind_address = {.s_addr = htonl(INADDR_LOOPBACK)};
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 < argc) {
            if (strcmp(argv[i], "--port") == 0 && parse_port(argv[i + 1], &port)) {
                continue;
            }
            if (strcmp(argv[i], "--bind-address") == 0 &&
                inet_pton(AF_INET, argv[i + 1], &bind_address) == 1) {
                continue;
            }
        }
        fprintf(stderr, "Usage: %s [--port PORT] [--bind-address IPV4]\n", argv[0]);
        return EXIT_FAILURE;
    }
    const char seed[] = "방명록에 오신 것을 환영합니다!";
    (void)store_message((struct http_body){seed, sizeof seed - 1});
    int result = serve_tcp("backend", bind_address.s_addr, port,
                           handle_backend_connection, NULL);
    pthread_mutex_destroy(&guestbook.mutex);
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
