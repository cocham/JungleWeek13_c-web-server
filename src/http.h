#ifndef HTTP_H
#define HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    HTTP_HEADER_LIMIT = 8192,
    HTTP_REQUEST_BODY_LIMIT = 4096,
    HTTP_TARGET_LIMIT = 2048,
    HTTP_UPSTREAM_BODY_LIMIT = 65536
};

struct http_body {
    const char *data;
    size_t length;
};

struct request {
    // All returned views borrow storage from this object.
    char bytes[HTTP_HEADER_LIMIT + HTTP_REQUEST_BODY_LIMIT];
    const char *method;
    // Always origin-form: an absolute-form target is rewritten to its path and query.
    const char *target;
    const char *content_type;
    struct http_body body;
    // Set as soon as the method is parsed, so even error replies to HEAD omit content.
    bool head;
};

struct upstream_response {
    char bytes[HTTP_HEADER_LIMIT + HTTP_UPSTREAM_BODY_LIMIT];
    int status;
    const char *content_type;
    struct http_body body;
};

enum upstream_result { UPSTREAM_OK, UPSTREAM_INVALID, UPSTREAM_TIMEOUT };

// 200 means a complete request, 0 means no reply is possible, otherwise reply with that status.
int read_request(int fd, struct request *request, int64_t deadline);
enum upstream_result read_upstream_response(int fd, struct upstream_response *response,
                                            int64_t deadline);
size_t request_path_length(const struct request *request);

int send_response_head(int fd, int status, const char *content_type,
                       size_t length, const char *allow, int64_t deadline);
int send_response(int fd, int status, const char *content_type,
                  struct http_body body, int64_t deadline);
int send_error(int fd, int status, const char *allow, int64_t deadline);
// Like send_response()/send_error(), but send only the head when the request was HEAD.
int send_response_for(int fd, const struct request *request, int status,
                      const char *content_type, struct http_body body, int64_t deadline);
int send_error_for(int fd, const struct request *request, int status,
                   const char *allow, int64_t deadline);

#endif
