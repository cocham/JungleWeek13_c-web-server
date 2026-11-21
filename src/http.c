#include "http.h"
#include "tcp.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

enum wire_result { WIRE_OK, WIRE_EOF, WIRE_INVALID, WIRE_LIMIT, WIRE_TIMEOUT, WIRE_IO };

struct fields {
    const char *host;
    const char *length;
    const char *content_type;
    bool connection_content_type;
    bool transfer_encoding;
    bool expect;
};

// Return -1 for invalid syntax, 1 for a number above max, and 0 for success.
static int parse_decimal(const char *text, size_t max, size_t *number)
{
    size_t value = 0;
    bool above_max = false;
    if (*text == '\0') {
        return -1;
    }
    for (; *text != '\0'; ++text) {
        if (*text < '0' || *text > '9') {
            return -1;
        }
        size_t digit = (size_t)(*text - '0');
        if (value > max / 10 || (value == max / 10 && digit > max % 10)) {
            above_max = true;
        }
        if (!above_max) {
            value = value * 10 + digit;
        }
    }
    if (above_max) {
        return 1;
    }
    *number = value;
    return 0;
}

static bool valid_token(const char *text)
{
    if (*text == '\0') {
        return false;
    }
    for (; *text != '\0'; ++text) {
        char ch = *text;
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || strchr("!#$%&'*+-.^_\x60|~", ch) != NULL)) {
            return false;
        }
    }
    return true;
}

// Host and an absolute-form authority share the grammar uri-host [ ":" port ] (RFC 9110 7.2).
static bool valid_host(const char *text, size_t length)
{
    if (length == 0) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        if (strchr(" \t,/?#@\\", text[i]) != NULL) {
            return false;
        }
    }
    return true;
}

// RFC 9112 3.2.2: an origin server MUST accept absolute-form and use its authority instead
// of Host. Rewrite it in place to the origin-form that routing and proxying expect.
static bool take_origin_form(char **target)
{
    char *text = *target;
    if (text[0] == '/') {
        return true;
    }
    if (strncasecmp(text, "http://", sizeof "http://" - 1) != 0) {
        return false;
    }
    char *authority = text + sizeof "http://" - 1;
    size_t length = strcspn(authority, "/?");
    if (!valid_host(authority, length)) {
        return false;
    }
    char *path = authority + length;
    if (*path != '/') {
        // An empty path means "/" (RFC 9110 4.2.3); the authority's last byte is free for it.
        *--path = '/';
    }
    *target = path;
    return true;
}

static int parse_request_line(char *line, struct request *request)
{
    char *target = strchr(line, ' ');
    if (target == NULL) {
        return 400;
    }
    *target++ = '\0';
    char *version = strchr(target, ' ');
    if (version == NULL) {
        return 400;
    }
    *version++ = '\0';
    if (!valid_token(line)) {
        return 400;
    }
    request->method = line;
    request->head = strcmp(line, "HEAD") == 0;
    if (strlen(target) > HTTP_TARGET_LIMIT) {
        return 414;
    }
    for (const unsigned char *byte = (const unsigned char *)target; *byte; ++byte) {
        if (*byte <= ' ' || *byte >= 127 || *byte == '#') {
            return 400;
        }
    }
    if (strlen(version) != 8 || strncmp(version, "HTTP/", 5) != 0 ||
        version[5] < '0' || version[5] > '9' || version[6] != '.' ||
        version[7] < '0' || version[7] > '9') {
        return 400;
    }
    if (strcmp(version, "HTTP/1.1") != 0) {
        return 505;
    }
    if (!take_origin_form(&target)) {
        return 400;
    }
    request->target = target;
    return 200;
}

static char *take_line(char **cursor)
{
    char *line = *cursor;
    char *end = strstr(line, "\r\n");
    if (end == NULL) {
        return NULL;
    }
    *end = '\0';
    *cursor = end + 2;
    return line;
}

static bool valid_field_bytes(const char *value)
{
    for (const unsigned char *byte = (const unsigned char *)value; *byte; ++byte) {
        if ((*byte < ' ' && *byte != '\t') || *byte == 127) {
            return false;
        }
    }
    return true;
}

static char *take_field_value(char *line)
{
    char *value = strchr(line, ':');
    if (value == NULL) {
        return NULL;
    }
    *value++ = '\0';
    if (!valid_token(line) || !valid_field_bytes(value)) {
        return NULL;
    }
    value += strspn(value, " \t");
    char *end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t')) {
        --end;
    }
    *end = '\0';
    return value;
}

static bool connection_names_content_type(const char *value)
{
    while (*value != '\0') {
        value += strspn(value, " \t");
        size_t length = strcspn(value, ",");
        const char *next = value + length;
        while (length > 0 && (value[length - 1] == ' ' || value[length - 1] == '\t')) {
            --length;
        }
        if (length == sizeof "Content-Type" - 1 &&
            strncasecmp(value, "Content-Type", length) == 0) {
            return true;
        }
        value = *next == ',' ? next + 1 : next;
    }
    return false;
}

static bool parse_fields(char *cursor, struct fields *fields)
{
    while (*cursor != '\0') {
        char *name = take_line(&cursor);
        if (name == NULL) {
            return false;
        }
        const char *value = take_field_value(name);
        if (value == NULL) {
            return false;
        }
        const char **single = NULL;
        if (strcasecmp(name, "Host") == 0) {
            single = &fields->host;
        } else if (strcasecmp(name, "Content-Length") == 0) {
            single = &fields->length;
        } else if (strcasecmp(name, "Content-Type") == 0) {
            single = &fields->content_type;
        } else if (strcasecmp(name, "Connection") == 0 && connection_names_content_type(value)) {
            fields->connection_content_type = true;
        } else if (strcasecmp(name, "Transfer-Encoding") == 0) {
            fields->transfer_encoding = true;
        } else if (strcasecmp(name, "Expect") == 0) {
            fields->expect = true;
        }
        if (single != NULL) {
            if (*single != NULL) {
                return false;
            }
            *single = value;
        }
    }
    return true;
}

static enum wire_result read_head(int fd, char *bytes, size_t *used,
                                  size_t *head_length, int64_t deadline)
{
    while (*used < HTTP_HEADER_LIMIT) {
        size_t scan = *used > 3 ? *used - 3 : 0;
        ssize_t received = receive_bytes(fd, bytes + *used, HTTP_HEADER_LIMIT - *used, deadline);
        if (received < 0) {
            return errno == ETIMEDOUT ? WIRE_TIMEOUT : WIRE_IO;
        }
        if (received == 0) {
            return *used == 0 ? WIRE_EOF : WIRE_INVALID;
        }
        *used += (size_t)received;
        for (; scan + 4 <= *used; ++scan) {
            if (memcmp(bytes + scan, "\r\n\r\n", 4) == 0) {
                *head_length = scan + 4;
                if (memchr(bytes, '\0', *head_length) != NULL) {
                    return WIRE_INVALID;
                }
                // Terminate the header at the empty line, preserving prefetched body bytes.
                bytes[scan + 2] = '\0';
                return WIRE_OK;
            }
        }
    }
    return WIRE_LIMIT;
}

static enum wire_result read_body(int fd, char *bytes, size_t used,
                                  size_t end, int64_t deadline)
{
    while (used < end) {
        ssize_t received = receive_bytes(fd, bytes + used, end - used, deadline);
        if (received < 0) {
            return errno == ETIMEDOUT ? WIRE_TIMEOUT : WIRE_IO;
        }
        if (received == 0) {
            return WIRE_INVALID;
        }
        used += (size_t)received;
    }
    return WIRE_OK;
}

static int request_status(enum wire_result result)
{
    switch (result) {
        case WIRE_OK: return 200;
        case WIRE_INVALID: return 400;
        case WIRE_LIMIT: return 431;
        case WIRE_TIMEOUT: return 408;
        case WIRE_EOF:
        case WIRE_IO: return 0;
    }
    return 500;
}

// RFC 9112 3 says a too-long target MUST get 414, so a full header buffer is not always 431.
static int oversized_head_status(struct request *request, size_t used)
{
    char *bytes = request->bytes;
    size_t end = 0;
    while (end + 1 < used && (bytes[end] != '\r' || bytes[end + 1] != '\n')) {
        ++end;
    }
    if (end + 1 >= used) {
        return 414; // The request line alone did not fit.
    }
    if (memchr(bytes, '\0', end) != NULL) {
        return 400;
    }
    bytes[end] = '\0';
    int status = parse_request_line(bytes, request);
    return status == 200 ? 431 : status;
}

int read_request(int fd, struct request *request, int64_t deadline)
{
    size_t used = 0;
    size_t head_length = 0;
    request->head = false;
    enum wire_result result = read_head(fd, request->bytes, &used, &head_length, deadline);
    if (result == WIRE_LIMIT) {
        return oversized_head_status(request, used);
    }
    if (result != WIRE_OK) {
        return request_status(result);
    }
    char *cursor = request->bytes;
    char *line = take_line(&cursor);
    int status = line == NULL ? 400 : parse_request_line(line, request);
    if (status != 200) {
        return status;
    }
    struct fields fields = {0};
    // RFC 9112 3.2: Host is still required and validated, even when absolute-form overrides it.
    if (!parse_fields(cursor, &fields) || fields.host == NULL ||
        !valid_host(fields.host, strlen(fields.host)) ||
        (fields.length != NULL && fields.transfer_encoding)) {
        return 400;
    }
    size_t length = 0;
    if (fields.length != NULL) {
        int parsed = parse_decimal(fields.length, HTTP_REQUEST_BODY_LIMIT, &length);
        if (parsed != 0) {
            return parsed < 0 ? 400 : 413;
        }
    }
    if (fields.transfer_encoding) {
        return 501;
    }
    if (fields.expect) {
        return 417;
    }
    if (strcmp(request->method, "POST") == 0 && fields.length == NULL) {
        return 411;
    }
    result = read_body(fd, request->bytes, used, head_length + length, deadline);
    if (result != WIRE_OK) {
        return request_status(result);
    }
    request->content_type = fields.connection_content_type ? NULL : fields.content_type;
    request->body = (struct http_body){request->bytes + head_length, length};
    return 200;
}

size_t request_path_length(const struct request *request)
{
    return strcspn(request->target, "?");
}

static int parse_status_line(const char *line)
{
    if (line == NULL || strlen(line) < 13 || strncmp(line, "HTTP/1.1 ", 9) != 0 ||
        strspn(line + 9, "0123456789") != 3 || line[12] != ' ' ||
        !valid_field_bytes(line + 13)) {
        return 0;
    }
    int status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + line[11] - '0';
    return status >= 200 && status <= 599 && status != 204 && status != 304 ? status : 0;
}

enum upstream_result read_upstream_response(int fd, struct upstream_response *response,
                                            int64_t deadline)
{
    size_t used = 0;
    size_t head_length = 0;
    enum wire_result result = read_head(fd, response->bytes, &used, &head_length, deadline);
    if (result != WIRE_OK) {
        return result == WIRE_TIMEOUT ? UPSTREAM_TIMEOUT : UPSTREAM_INVALID;
    }
    char *cursor = response->bytes;
    int status = parse_status_line(take_line(&cursor));
    struct fields fields = {0};
    size_t length;
    if (status == 0 || !parse_fields(cursor, &fields) || fields.transfer_encoding ||
        fields.length == NULL ||
        parse_decimal(fields.length, HTTP_UPSTREAM_BODY_LIMIT, &length) != 0 ||
        used > head_length + length) {
        return UPSTREAM_INVALID;
    }
    result = read_body(fd, response->bytes, used, head_length + length, deadline);
    if (result != WIRE_OK) {
        return result == WIRE_TIMEOUT ? UPSTREAM_TIMEOUT : UPSTREAM_INVALID;
    }
    response->status = status;
    response->body = (struct http_body){response->bytes + head_length, length};
    response->content_type = fields.connection_content_type ||
                             fields.content_type == NULL || *fields.content_type == '\0'
                               ? "application/octet-stream" : fields.content_type;
    return UPSTREAM_OK;
}

static const char *reason_phrase(int status)
{
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 417: return "Expectation Failed";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        default: return "";
    }
}

int send_response_head(int fd, int status, const char *content_type,
                       size_t length, const char *allow, int64_t deadline)
{
    char header[HTTP_HEADER_LIMIT + 256];
    int size = snprintf(header, sizeof header,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Connection: close\r\n%s%s%s\r\n",
        status, reason_phrase(status), content_type, length,
        allow == NULL ? "" : "Allow: ", allow == NULL ? "" : allow,
        allow == NULL ? "" : "\r\n");
    if (size < 0 || (size_t)size >= sizeof header) {
        return -1;
    }
    return send_all(fd, header, (size_t)size, deadline);
}

int send_response(int fd, int status, const char *content_type,
                  struct http_body body, int64_t deadline)
{
    if (send_response_head(fd, status, content_type, body.length, NULL, deadline) < 0) {
        return -1;
    }
    return send_all(fd, body.data, body.length, deadline);
}

static int send_error_reply(int fd, int status, const char *allow, bool head, int64_t deadline)
{
    char body[96];
    int length = snprintf(body, sizeof body, "%s\n", reason_phrase(status));
    if (length < 0 || (size_t)length >= sizeof body ||
        send_response_head(fd, status, "text/plain; charset=utf-8",
                           (size_t)length, allow, deadline) < 0) {
        return -1;
    }
    return head ? 0 : send_all(fd, body, (size_t)length, deadline);
}

int send_error(int fd, int status, const char *allow, int64_t deadline)
{
    return send_error_reply(fd, status, allow, false, deadline);
}

// RFC 9110 9.3.2: a HEAD response has the same header fields as GET but MUST NOT carry content.
int send_response_for(int fd, const struct request *request, int status,
                      const char *content_type, struct http_body body, int64_t deadline)
{
    if (request->head) {
        return send_response_head(fd, status, content_type, body.length, NULL, deadline);
    }
    return send_response(fd, status, content_type, body, deadline);
}

int send_error_for(int fd, const struct request *request, int status,
                   const char *allow, int64_t deadline)
{
    return send_error_reply(fd, status, allow, request->head, deadline);
}
