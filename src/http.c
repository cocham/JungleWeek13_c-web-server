#include "http.h"
#include "tcp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
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
    bool expect_continue;
    bool expect_other;
};

// 10진수 문자열을 max 이하의 size_t로 바꾼다.
// 숫자가 아닌 문자가 있으면 -1, max를 넘으면 1, 성공하면 0을 반환한다.
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

// ASCII 영문자 또는 숫자인지 확인한다. 로캘의 영향을 받는 isalnum() 대신 쓴다.
static bool is_alnum(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
}

// 16진수 숫자(0-9, A-F, a-f)인지 확인한다. 퍼센트 인코딩 %XX 검사에 쓴다.
static bool is_hex(char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f');
}

// 메서드와 필드 이름이 token 문법(1*tchar)을 따르는지 확인한다 (RFC 9110 §5.6.2).
static bool valid_token(const char *text)
{
    if (*text == '\0') {
        return false;
    }
    for (; *text != '\0'; ++text) {
        if (!is_alnum(*text) && strchr("!#$%&'*+-.^_\x60|~", *text) == NULL) {
            return false;
        }
    }
    return true;
}

// RFC 3986 §2.2-2.3의 unreserved 또는 sub-delims 문자인지 확인한다.
// Host의 reg-name과 요청 대상의 pchar 검사가 함께 쓴다.
static bool unreserved_or_sub_delim(char ch)
{
    return is_alnum(ch) || (ch != '\0' && strchr("-._~!$&'()*+,;=", ch) != NULL);
}

// Host 값이나 absolute-form의 authority가 uri-host [ ":" port ] 문법을 따르는지
// 확인한다 (RFC 9110 §7.2, RFC 3986 §3.2.2-3.2.3). IPv6 리터럴은 inet_pton()으로 확인하고,
// http URI의 호스트는 비어 있으면 안 된다 (RFC 9110 §4.2.1).
static bool valid_host(const char *text, size_t length)
{
    size_t end = 0;
    if (length > 0 && text[0] == '[') {
        // IP-literal: only IPv6 is accepted; IPvFuture has no registered use.
        const char *close = memchr(text, ']', length);
        char address[INET6_ADDRSTRLEN];
        struct in6_addr parsed;
        if (close == NULL || (size_t)(close - text) - 1 >= sizeof address) {
            return false;
        }
        size_t inner = (size_t)(close - text) - 1;
        memcpy(address, text + 1, inner);
        address[inner] = '\0';
        if (inet_pton(AF_INET6, address, &parsed) != 1) {
            return false;
        }
        end = inner + 2;
    } else {
        // reg-name, which also covers IPv4address.
        while (end < length && text[end] != ':') {
            if (text[end] == '%') {
                if (end + 2 >= length || !is_hex(text[end + 1]) || !is_hex(text[end + 2])) {
                    return false;
                }
                end += 3;
            } else if (unreserved_or_sub_delim(text[end])) {
                ++end;
            } else {
                return false;
            }
        }
        if (end == 0) {
            return false;
        }
    }
    if (end == length) {
        return true;
    }
    if (text[end] != ':') {
        return false;
    }
    for (size_t i = end + 1; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
    }
    return true;
}

// 요청 대상이 origin-form(absolute-path [ "?" query ])인지 확인한다 (RFC 9112 §3.2.1).
// pchar, "/", "?"와 올바른 %XX만 허용한다 (RFC 3986 §3.3-3.4).
static bool valid_origin_form(const char *text)
{
    if (*text != '/') {
        return false;
    }
    for (; *text != '\0'; ++text) {
        if (*text == '%') {
            if (!is_hex(text[1]) || !is_hex(text[2])) {
                return false;
            }
            text += 2;
        } else if (!unreserved_or_sub_delim(*text) && strchr("/?:@", *text) == NULL) {
            return false;
        }
    }
    return true;
}

// absolute-form(http://host/path) 대상을 그 자리에서 origin-form(/path)으로 바꾼다.
// 서버는 absolute-form을 받아야 하고 Host 대신 대상의 authority를 써야 한다 (RFC 9112 §3.2.2).
// 이미 origin-form이면 그대로 두고, 다른 스킴이나 잘못된 authority면 false를 반환한다.
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

// 서버가 구현한 메서드(GET, HEAD, POST)인지 확인한다.
// 앞단과 백엔드 모두 이 셋만 구현하며, 나머지는 501로 응답한다 (RFC 9110 §9.1).
static bool implemented_method(const char *text, size_t length)
{
    return (length == 3 && memcmp(text, "GET", 3) == 0) ||
           (length == 4 && (memcmp(text, "HEAD", 4) == 0 || memcmp(text, "POST", 4) == 0));
}

// NUL로 끝나는 요청 줄을 메서드, 대상, 버전으로 나눠 request에 채운다.
// absolute-form 대상은 origin-form으로 바꾼다. 성공하면 200, 아니면 응답할
// 상태 코드(400, 501, 505)를 반환한다.
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
    if (!implemented_method(line, strlen(line))) {
        return 501;
    }
    if (strlen(version) != 8 || strncmp(version, "HTTP/", 5) != 0 ||
        version[5] < '0' || version[5] > '9' || version[6] != '.' ||
        version[7] < '0' || version[7] > '9') {
        return 400;
    }
    // RFC 9110 2.5: a higher 1.x minor version is handled as 1.1; 505 is for other majors.
    if (version[5] != '1') {
        return 505;
    }
    request->minor_version = version[7] - '0';
    if (!take_origin_form(&target) || !valid_origin_form(target)) {
        return 400;
    }
    request->target = target;
    return 200;
}

// cursor 위치의 한 줄을 CRLF에서 잘라 반환하고 cursor를 다음 줄로 옮긴다.
// CRLF가 없으면 NULL을 반환한다.
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

// 필드 값에 HTAB을 뺀 제어 문자나 DEL이 없는지 확인한다 (RFC 9110 §5.5).
static bool valid_field_bytes(const char *value)
{
    for (const unsigned char *byte = (const unsigned char *)value; *byte; ++byte) {
        if ((*byte < ' ' && *byte != '\t') || *byte == 127) {
            return false;
        }
    }
    return true;
}

// "이름: 값" 한 줄을 이름과 값으로 나누고, 값 앞뒤의 공백(OWS)을 잘라 반환한다.
// 이름이 token이 아니거나 콜론 앞에 공백이 있으면 NULL이다 (RFC 9112 §5.1).
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

// 쉼표로 구분된 목록(RFC 9110 §5.6.1)에서 다음 항목을 꺼내 member와 length에 담는다.
// 항목 앞뒤 공백은 잘라내고 빈 항목은 길이 0으로 돌려준다. 남은 항목이 없으면 false다.
static bool next_member(const char **cursor, const char **member, size_t *length)
{
    const char *text = *cursor;
    if (*text == '\0') {
        return false;
    }
    text += strspn(text, " \t");
    size_t size = strcspn(text, ",");
    *cursor = text[size] == ',' ? text + size + 1 : text + size;
    while (size > 0 && (text[size - 1] == ' ' || text[size - 1] == '\t')) {
        --size;
    }
    *member = text;
    *length = size;
    return true;
}

// Connection 헤더가 Content-Type을 연결별 헤더로 지정했는지 확인한다.
// 그렇다면 Content-Type은 다음 홉으로 넘기지 않는다 (RFC 9110 §7.6.1).
static bool connection_names_content_type(const char *value)
{
    const char *member;
    size_t length;
    while (next_member(&value, &member, &length)) {
        if (length == sizeof "Content-Type" - 1 &&
            strncasecmp(member, "Content-Type", length) == 0) {
            return true;
        }
    }
    return false;
}

// Expect 헤더의 항목을 100-continue와 그 밖의 기대값으로 나눠 fields에 기록한다.
// 정의된 기대값은 100-continue뿐이고 대소문자를 구분하지 않는다 (RFC 9110 §10.1.1).
static void note_expectations(const char *value, struct fields *fields)
{
    const char *member;
    size_t length;
    while (next_member(&value, &member, &length)) {
        if (length == sizeof "100-continue" - 1 &&
            strncasecmp(member, "100-continue", length) == 0) {
            fields->expect_continue = true;
        } else if (length > 0) { // Empty list members are ignored.
            fields->expect_other = true;
        }
    }
}

// 헤더 필드 줄을 차례로 읽어 이 서버가 쓰는 필드만 fields에 기록한다.
// 형식이 틀린 줄이 있거나 Host, Content-Length, Content-Type이 두 번 나오면 false다.
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
            note_expectations(value, fields);
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

// 헤더 끝을 알리는 빈 줄(CRLF CRLF)이 나올 때까지 limit 바이트 안에서 읽는다.
// 헤더와 함께 도착한 본문 바이트는 버퍼에 그대로 둔다. request가 true면
// 요청 줄 앞의 빈 줄을 버린다 (RFC 9112 §2.2). 다 채우도록 못 찾으면 WIRE_LIMIT다.
static enum wire_result read_head(int fd, char *bytes, size_t limit, bool request,
                                  size_t *used, size_t *head_length, int64_t deadline)
{
    size_t scan = 0;
    while (*used < limit) {
        ssize_t received = receive_bytes(fd, bytes + *used, limit - *used, deadline);
        if (received < 0) {
            return errno == ETIMEDOUT ? WIRE_TIMEOUT : WIRE_IO;
        }
        if (received == 0) {
            return *used == 0 ? WIRE_EOF : WIRE_INVALID;
        }
        *used += (size_t)received;
        if (request) {
            // RFC 9112 2.2: a server SHOULD ignore at least one empty line before the request-line.
            size_t skip = 0;
            while (skip + 1 < *used && bytes[skip] == '\r' && bytes[skip + 1] == '\n') {
                skip += 2;
            }
            if (skip > 0) {
                memmove(bytes, bytes + skip, *used - skip);
                *used -= skip;
                scan = 0;
            }
        }
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

// 버퍼의 used 위치부터 end까지 남은 본문을 끝까지 읽는다.
// 도중에 연결이 끊기면 WIRE_INVALID를 반환한다.
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

// 읽기 결과를 클라이언트에 보낼 상태 코드로 바꾼다. 0은 응답할 수 없다는 뜻이다.
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

// 요청 줄이 8000 옥텟 상한을 넘었을 때 보낼 상태 코드를 정한다 (RFC 9112 §3).
// 구현한 메서드면 대상이 긴 것이므로 414(MUST), 아니면 메서드가 긴 것이므로 501(SHOULD)이다.
static int overlong_line_status(struct request *request, size_t used)
{
    const char *bytes = request->bytes;
    size_t method = 0;
    while (method < used && bytes[method] != ' ') {
        ++method;
    }
    request->head = method == 4 && memcmp(bytes, "HEAD", 4) == 0;
    return implemented_method(bytes, method) ? 414 : 501;
}

// 클라이언트 요청 하나를 요청 줄, 헤더, 본문 순서로 읽고 검증해 request에 채운다.
// 200이면 처리할 요청, 0이면 응답할 수 없는 상태, 그 밖에는 그 상태 코드로 응답한다.
// 100-continue 요청이면 본문을 읽기 전에 100 Continue를 먼저 보낸다.
int read_request(int fd, struct request *request, int64_t deadline)
{
    char *bytes = request->bytes;
    size_t used = 0;
    size_t head_length = 0;
    request->head = false;
    request->minor_version = 1;
    enum wire_result result = read_head(fd, bytes, HTTP_REQUEST_HEAD_LIMIT, true,
                                        &used, &head_length, deadline);
    if (result != WIRE_OK && result != WIRE_LIMIT) {
        return request_status(result);
    }
    size_t line_length = 0;
    while (line_length + 1 < used &&
           (bytes[line_length] != '\r' || bytes[line_length + 1] != '\n')) {
        ++line_length;
    }
    if (line_length + 1 >= used || line_length > HTTP_REQUEST_LINE_LIMIT) {
        return overlong_line_status(request, used);
    }
    if (memchr(bytes, '\0', line_length) != NULL) {
        return 400;
    }
    bytes[line_length] = '\0';
    int status = parse_request_line(bytes, request);
    if (status != 200) {
        return status;
    }
    // The request line fits, so whatever overflowed is the field section.
    if (result == WIRE_LIMIT || head_length - (line_length + 2) > HTTP_HEADER_LIMIT) {
        return 431;
    }
    struct fields fields = {0};
    // RFC 9112 3.2: HTTP/1.1 requires exactly one valid Host, even when absolute-form
    // overrides it. A Host sent with HTTP/1.0 must still be valid.
    if (!parse_fields(bytes + line_length + 2, &fields) ||
        (fields.host == NULL && request->minor_version >= 1) ||
        (fields.host != NULL && !valid_host(fields.host, strlen(fields.host))) ||
        (fields.length != NULL && fields.transfer_encoding) ||
        // RFC 9112 6.1: Transfer-Encoding in HTTP/1.0 MUST be treated as faulty framing.
        (fields.transfer_encoding && request->minor_version == 0)) {
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
    if (fields.expect_other) {
        return 417;
    }
    if (strcmp(request->method, "POST") == 0 && fields.length == NULL) {
        return 411;
    }
    // RFC 9110 10.1.1: invite the content of an HTTP/1.1 100-continue request once every
    // header-only check has passed; HTTP/1.0 MUST ignore the expectation.
    if (fields.expect_continue && request->minor_version >= 1 && used < head_length + length) {
        static const char interim[] = "HTTP/1.1 100 Continue\r\n\r\n";
        if (send_all(fd, interim, sizeof interim - 1, deadline) < 0) {
            return 0;
        }
    }
    result = read_body(fd, bytes, used, head_length + length, deadline);
    if (result != WIRE_OK) {
        return request_status(result);
    }
    request->content_type = fields.connection_content_type ? NULL : fields.content_type;
    request->body = (struct http_body){bytes + head_length, length};
    return 200;
}

// 요청 대상에서 질의(?) 앞까지의 경로 길이를 반환한다.
size_t request_path_length(const struct request *request)
{
    return strcspn(request->target, "?");
}

// 백엔드 응답의 상태 줄을 검사하고 상태 코드를 반환한다.
// 형식이 틀렸거나 기준 프록시가 다루지 않는 1xx, 204, 304면 0이다.
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

// 백엔드 응답 하나를 상태 줄, 헤더, Content-Length만큼의 본문까지 읽고 검증한다.
// 시간 초과는 UPSTREAM_TIMEOUT(504로 응답), 나머지 오류는 UPSTREAM_INVALID(502)다.
enum upstream_result read_upstream_response(int fd, struct upstream_response *response,
                                            int64_t deadline)
{
    size_t used = 0;
    size_t head_length = 0;
    enum wire_result result = read_head(fd, response->bytes, HTTP_HEADER_LIMIT, false,
                                        &used, &head_length, deadline);
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

// 상태 코드에 맞는 이유 문구를 반환한다. 모르는 코드는 빈 문자열이다.
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

// 상태 줄과 Content-Type, Content-Length, Connection: close, 필요하면 Allow까지
// 헤더만 보낸다. 본문은 호출한 쪽이 이어서 보낸다.
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

// 헤더를 보낸 뒤 본문 전체를 보낸다.
int send_response(int fd, int status, const char *content_type,
                  struct http_body body, int64_t deadline)
{
    if (send_response_head(fd, status, content_type, body.length, NULL, deadline) < 0) {
        return -1;
    }
    return send_all(fd, body.data, body.length, deadline);
}

// 이유 문구 한 줄을 본문으로 하는 오류 응답을 보낸다. head가 true면 헤더만 보낸다.
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

// 요청과 관계없이 오류 응답을 본문까지 보낸다. 요청이 없는 곳(예: 503 과부하)에서 쓴다.
int send_error(int fd, int status, const char *allow, int64_t deadline)
{
    return send_error_reply(fd, status, allow, false, deadline);
}

// 요청이 HEAD면 헤더만, 아니면 본문까지 보낸다.
// HEAD 응답은 GET과 같은 헤더를 보내되 본문을 담으면 안 된다 (RFC 9110 §9.3.2).
int send_response_for(int fd, const struct request *request, int status,
                      const char *content_type, struct http_body body, int64_t deadline)
{
    if (request->head) {
        return send_response_head(fd, status, content_type, body.length, NULL, deadline);
    }
    return send_response(fd, status, content_type, body, deadline);
}

// send_error()와 같지만 요청이 HEAD면 오류 응답도 헤더만 보낸다.
int send_error_for(int fd, const struct request *request, int status,
                   const char *allow, int64_t deadline)
{
    return send_error_reply(fd, status, allow, request->head, deadline);
}
