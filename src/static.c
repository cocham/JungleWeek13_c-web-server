#include "static.h"
#include "tcp.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static int hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static int decode_path(const struct request *request, char *path)
{
    size_t end = request_path_length(request);
    if (end == 1) {
        strcpy(path, "index.html");
        return 200;
    }

    size_t decoded = 0;
    for (size_t i = 1; i < end; ++i) {
        unsigned char byte = (unsigned char)request->target[i];
        if (byte == '%') {
            if (end - i < 3) {
                return 400;
            }
            int high = hex_digit(request->target[i + 1]);
            int low = hex_digit(request->target[i + 2]);
            if (high < 0 || low < 0) {
                return 400;
            }
            byte = (unsigned char)(high * 16 + low);
            i += 2;
        }
        if (byte < ' ' || byte == 127 || byte == '\\') {
            return 403;
        }
        path[decoded++] = (char)byte;
    }
    path[decoded] = '\0';
    return 200;
}

static const char *mime_type(const char *path)
{
    static const struct {
        const char *extension;
        const char *content_type;
    } types[] = {
        {".html", "text/html; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"}
    };
    const char *extension = strrchr(path, '.');
    for (size_t i = 0; extension != NULL && i < sizeof types / sizeof types[0]; ++i) {
        if (strcasecmp(extension, types[i].extension) == 0) {
            return types[i].content_type;
        }
    }
    return "application/octet-stream";
}

static int file_open_status(int error)
{
    switch (error) {
        case ENOENT: return 404;
        case EACCES:
        case EPERM:
        case ENOTDIR:
        case ELOOP: return 403;
        default: return 500;
    }
}

static int open_static_file(int root_fd, char *path, int *file_fd)
{
    int directory = root_fd;
    int status = 403;
    for (;;) {
        char *slash = strchr(path, '/');
        if (slash != NULL) {
            *slash = '\0';
        }
        if (*path == '\0' || strcmp(path, ".") == 0 || strcmp(path, "..") == 0) {
            break;
        }
        int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;
        int next = openat(directory, path, flags | (slash == NULL ? 0 : O_DIRECTORY));
        if (next < 0) {
            status = file_open_status(errno);
            break;
        }
        if (slash == NULL) {
            *file_fd = next;
            status = 200;
            break;
        }
        if (directory != root_fd) {
            close(directory);
        }
        directory = next;
        path = slash + 1;
    }
    if (directory != root_fd) {
        close(directory);
    }
    return status;
}

static int send_static_file(int client_fd, const struct request *request, int file_fd,
                            const char *content_type, int64_t deadline)
{
    struct stat info;
    if (fstat(file_fd, &info) < 0) {
        return send_error_for(client_fd, request, 500, NULL, deadline);
    }
    if (!S_ISREG(info.st_mode)) {
        return send_error_for(client_fd, request, 403, NULL, deadline);
    }
    if (info.st_size < 0 || (uintmax_t)info.st_size > SIZE_MAX) {
        return send_error_for(client_fd, request, 500, NULL, deadline);
    }
    size_t remaining = (size_t)info.st_size;
    if (send_response_head(client_fd, 200, content_type, remaining, NULL, deadline) < 0) {
        return -1;
    }
    if (request->head) {
        return 0; // Same Content-Length as GET, no content (RFC 9110 9.3.2).
    }
    // 본문은 조각마다 CLIENT_WRITE_MS를 새로 준다. 응답 전체에 기한 하나를 걸면 큰 파일이나
    // 느린 클라이언트는 정상적으로 받고 있어도 중간에 끊긴다. 조각 하나를 그 시간 안에 받지
    // 못하는 클라이언트만 끊으므로 작업자를 무한정 붙잡히지도 않는다.
    char buffer[16384];
    while (remaining > 0) {
        size_t capacity = remaining < sizeof buffer ? remaining : sizeof buffer;
        ssize_t count = read(file_fd, buffer, capacity);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        // 0은 전송 도중 파일이 줄어든 경우다. Content-Length를 이미 보냈으니 연결을 끊는다.
        if (count <= 0 ||
            send_all(client_fd, buffer, (size_t)count, deadline_after(CLIENT_WRITE_MS)) < 0) {
            return -1;
        }
        remaining -= (size_t)count;
    }
    return 0;
}

int serve_static(int client_fd, int root_fd, const struct request *request, int64_t deadline)
{
    char path[HTTP_TARGET_LIMIT + 1];
    int status = decode_path(request, path);
    if (status != 200) {
        return send_error_for(client_fd, request, status, NULL, deadline);
    }
    const char *content_type = mime_type(path);
    int file_fd = -1;
    status = open_static_file(root_fd, path, &file_fd);
    if (status != 200) {
        return send_error_for(client_fd, request, status, NULL, deadline);
    }
    int result = send_static_file(client_fd, request, file_fd, content_type, deadline);
    close(file_fd);
    return result;
}
