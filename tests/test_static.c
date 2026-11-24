// 정적 파일 응답(serve_static)의 단위 테스트.
// 임시 문서 루트를 만들고 socketpair()의 한쪽을 클라이언트로 써서 응답 바이트를 그대로 검사한다.
// 경로 탐색·URL 디코딩·심볼릭 링크 우회, 404/403 정책, MIME, 큰 파일의 길이와 기한,
// 클라이언트가 중도에 끊었을 때 FD 회수를 확인한다.
#include "static.h"
#include "tcp.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// 소켓 버퍼(수백 KB)보다 훨씬 커서 전송 중에 여러 번 막히는 크기.
enum { BIG_SIZE = 5 * 1024 * 1024 + 123 };
// 응답 헤더를 받아 둘 여유.
enum { HEAD_ROOM = 1024 };

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

static void fail_setup(const char *what)
{
    perror(what);
    exit(2);
}

static void sleep_ms(int milliseconds)
{
    struct timespec pause = {milliseconds / 1000, (long)(milliseconds % 1000) * 1000000L};
    while (nanosleep(&pause, &pause) < 0 && errno == EINTR) {
    }
}

// ---------------------------------------------------------------------------
// 임시 문서 루트
//
// base/
//   outside/secret.txt          루트 밖 파일
//   root/                       문서 루트
//     index.html  style.css  app.js  logo.png  photo.JPG  photo.jpeg  notes.txt  README
//     big.bin                   BIG_SIZE 바이트
//     sub/page.html  sub/index.html   index가 있는 하위 디렉터리
//     empty/                    index가 없는 하위 디렉터리
//     odd/index.html/           index.html이라는 이름의 디렉터리
//     locked.html               권한 000
//     pipe                      FIFO
//     link-in.html -> index.html
//     link-out.txt -> ../outside/secret.txt
//     link-dir -> ../outside

static char base[] = "/tmp/test_static.XXXXXX";
static int root_fd = -1;

static void path_in(char *out, const char *relative)
{
    if (snprintf(out, PATH_MAX, "%s/%s", base, relative) >= PATH_MAX) {
        fprintf(stderr, "path too long\n");
        exit(2);
    }
}

static void write_file(const char *relative, const char *data, size_t length, mode_t mode)
{
    char path[PATH_MAX];
    path_in(path, relative);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail_setup(path);
    }
    while (length > 0) {
        ssize_t written = write(fd, data, length);
        if (written < 0) {
            fail_setup("write");
        }
        data += written;
        length -= (size_t)written;
    }
    close(fd);
    if (chmod(path, mode) < 0) {
        fail_setup("chmod");
    }
}

#define WRITE_TEXT(relative, literal) write_file((relative), (literal), sizeof(literal) - 1, 0644)

static void make_dir(const char *relative)
{
    char path[PATH_MAX];
    path_in(path, relative);
    if (mkdir(path, 0755) < 0) {
        fail_setup(path);
    }
}

static void make_link(const char *target, const char *relative)
{
    char path[PATH_MAX];
    path_in(path, relative);
    if (symlink(target, path) < 0) {
        fail_setup(path);
    }
}

static char *big_data;

static void build_root(void)
{
    if (mkdtemp(base) == NULL) {
        fail_setup("mkdtemp");
    }
    make_dir("outside");
    WRITE_TEXT("outside/secret.txt", "secret\n");
    make_dir("root");
    WRITE_TEXT("root/index.html", "<h1>index</h1>\n");
    WRITE_TEXT("root/style.css", "body{}\n");
    WRITE_TEXT("root/app.js", "1;\n");
    WRITE_TEXT("root/logo.png", "\x89PNG\r\n\x1a\n\0png");
    WRITE_TEXT("root/photo.JPG", "\xff\xd8\xff\0jpg");
    WRITE_TEXT("root/photo.jpeg", "\xff\xd8\xff\0jpeg");
    WRITE_TEXT("root/notes.txt", "notes\n");
    WRITE_TEXT("root/README", "readme\n");
    make_dir("root/sub");
    WRITE_TEXT("root/sub/page.html", "<p>sub</p>\n");
    WRITE_TEXT("root/sub/index.html", "<p>sub index</p>\n");
    make_dir("root/empty");
    make_dir("root/odd");
    make_dir("root/odd/index.html");
    write_file("root/locked.html", "locked", 6, 0000);
    char pipe_path[PATH_MAX];
    path_in(pipe_path, "root/pipe");
    if (mkfifo(pipe_path, 0644) < 0) {
        fail_setup("mkfifo");
    }
    make_link("index.html", "root/link-in.html");
    make_link("../outside/secret.txt", "root/link-out.txt");
    make_link("../outside", "root/link-dir");

    // NUL과 CRLF가 섞인 바이트 패턴으로 binary-safe 여부도 함께 본다.
    big_data = malloc(BIG_SIZE);
    if (big_data == NULL) {
        fail_setup("malloc");
    }
    for (size_t i = 0; i < BIG_SIZE; ++i) {
        big_data[i] = (char)((i * 131 + i / 4099) & 0xff);
    }
    write_file("root/big.bin", big_data, BIG_SIZE, 0644);

    char root[PATH_MAX];
    path_in(root, "root");
    root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root_fd < 0) {
        fail_setup(root);
    }
}

static void remove_root(void)
{
    static const char *const entries[] = {
        "root/link-dir", "root/link-out.txt", "root/link-in.html", "root/pipe",
        "root/locked.html", "root/sub/index.html", "root/sub/page.html", "root/big.bin",
        "root/README",
        "root/notes.txt", "root/photo.jpeg", "root/photo.JPG", "root/logo.png", "root/app.js",
        "root/style.css", "root/index.html", "outside/secret.txt",
    };
    for (size_t i = 0; i < sizeof entries / sizeof entries[0]; ++i) {
        char path[PATH_MAX];
        path_in(path, entries[i]);
        unlink(path);
    }
    static const char *const directories[] = {
        "root/odd/index.html", "root/odd", "root/empty", "root/sub", "root", "outside",
    };
    for (size_t i = 0; i < sizeof directories / sizeof directories[0]; ++i) {
        char path[PATH_MAX];
        path_in(path, directories[i]);
        rmdir(path);
    }
    rmdir(base);
    close(root_fd);
    free(big_data);
}

// ---------------------------------------------------------------------------
// 클라이언트 역할의 읽기 스레드

enum reader_mode {
    READ_ALL,       // EOF까지 모두 읽는다.
    READ_LATE,      // delay_ms를 기다린 뒤 EOF까지 읽는다.
    READ_THEN_QUIT, // quit_after 바이트를 읽고 연결을 닫는다.
    READ_NOTHING    // 아무것도 읽지 않고 release 파이프가 열릴 때까지 멈춰 있는다.
};

struct reader {
    int fd;
    enum reader_mode mode;
    int delay_ms;
    size_t quit_after;
    int release;
    char *bytes;
    size_t capacity;
    size_t used;
};

static void *reader_main(void *context)
{
    struct reader *reader = context;
    if (reader->mode == READ_LATE) {
        sleep_ms(reader->delay_ms);
    }
    if (reader->mode == READ_NOTHING) {
        char byte;
        while (read(reader->release, &byte, 1) < 0 && errno == EINTR) {
        }
        close(reader->fd);
        return NULL;
    }
    size_t limit = reader->mode == READ_THEN_QUIT ? reader->quit_after : reader->capacity;
    while (reader->used < limit) {
        ssize_t count = read(reader->fd, reader->bytes + reader->used, limit - reader->used);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        reader->used += (size_t)count;
    }
    close(reader->fd);
    return NULL;
}

// 응답 한 건을 해석한 결과. 헤더 값은 reply 안의 문자열을 가리킨다.
struct reply {
    int result;
    int status;
    char content_type[128];
    char location[256];
    long long content_length;
    const char *body;
    size_t body_length;
    int64_t elapsed_ms;
};

static struct request request;
static char *reply_bytes;
static size_t reply_capacity;

static void header_value(const char *head, const char *name, char *out, size_t capacity)
{
    out[0] = '\0';
    size_t name_length = strlen(name);
    for (const char *line = strstr(head, "\r\n"); line != NULL; line = strstr(line, "\r\n")) {
        line += 2;
        if (strncasecmp(line, name, name_length) == 0 && line[name_length] == ':') {
            const char *value = line + name_length + 1;
            value += strspn(value, " ");
            size_t length = strcspn(value, "\r");
            if (length >= capacity) {
                length = capacity - 1;
            }
            memcpy(out, value, length);
            out[length] = '\0';
            return;
        }
    }
}

static struct reply serve(const char *method, const char *target, enum reader_mode mode,
                          int value, int head_deadline_ms)
{
    snprintf(request.bytes, sizeof request.bytes, "%s", target);
    request.target = request.bytes;
    request.method = method;
    request.head = strcmp(method, "HEAD") == 0;
    request.minor_version = 1;
    request.content_type = NULL;
    request.body = (struct http_body){NULL, 0};

    int fds[2];
    int release[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0 || set_nonblocking(fds[0]) < 0) {
        fail_setup("socketpair");
    }
    if (mode == READ_NOTHING && pipe(release) < 0) {
        fail_setup("pipe");
    }
    struct reader reader = {
        .fd = fds[1], .mode = mode, .delay_ms = value, .quit_after = (size_t)value,
        .release = release[0], .bytes = reply_bytes, .capacity = reply_capacity - 1,
    };
    pthread_t thread;
    if (pthread_create(&thread, NULL, reader_main, &reader) != 0) {
        fail_setup("pthread_create");
    }
    int64_t started = monotonic_ms();
    struct reply reply = {.content_length = -1};
    reply.result = serve_static(fds[0], root_fd, &request, deadline_after(head_deadline_ms));
    reply.elapsed_ms = monotonic_ms() - started;
    close(fds[0]);
    if (mode == READ_NOTHING) {
        close(release[1]);
    }
    pthread_join(thread, NULL);
    if (mode == READ_NOTHING) {
        close(release[0]);
    }

    reply_bytes[reader.used] = '\0';
    char *end = strstr(reply_bytes, "\r\n\r\n");
    if (end != NULL && sscanf(reply_bytes, "HTTP/1.1 %d", &reply.status) == 1) {
        *end = '\0';
        header_value(reply_bytes, "Content-Type", reply.content_type, sizeof reply.content_type);
        header_value(reply_bytes, "Location", reply.location, sizeof reply.location);
        char length[32];
        header_value(reply_bytes, "Content-Length", length, sizeof length);
        if (length[0] != '\0') {
            reply.content_length = atoll(length);
        }
        reply.body = end + 4;
        reply.body_length = reader.used - (size_t)(reply.body - reply_bytes);
    }
    return reply;
}

static struct reply get(const char *target)
{
    return serve("GET", target, READ_ALL, 0, CLIENT_WRITE_MS);
}

static int open_fd_count(void)
{
    DIR *directory = opendir("/proc/self/fd");
    if (directory == NULL) {
        return -1;
    }
    int count = 0;
    while (readdir(directory) != NULL) {
        ++count;
    }
    closedir(directory);
    return count;
}

// ---------------------------------------------------------------------------
// 경로 탐색·URL 디코딩·심볼릭 링크 우회

static void expect_status(const char *target, int expected, int line)
{
    struct reply reply = get(target);
    ++checks;
    if (reply.status != expected) {
        ++failures;
        fprintf(stderr, "test_static.c:%d: GET %s: expected %d, got %d\n", line, target,
                expected, reply.status);
    }
}

#define EXPECT_STATUS(target, expected) expect_status((target), (expected), __LINE__)

static void test_root_boundary(void)
{
    // 루트를 벗어나려는 경로. 원문이든 %XX든 ".." 조각은 루트 안을 가리켜도 거절한다.
    EXPECT_STATUS("/../outside/secret.txt", 403);
    EXPECT_STATUS("/%2e%2e/outside/secret.txt", 403);
    EXPECT_STATUS("/%2E%2E%2Foutside%2Fsecret.txt", 403);
    EXPECT_STATUS("/..%2foutside%2fsecret.txt", 403);
    EXPECT_STATUS("/sub/../index.html", 403);
    EXPECT_STATUS("/sub/%2e%2e/index.html", 403);
    EXPECT_STATUS("/./index.html", 403);
    EXPECT_STATUS("/%2e/index.html", 403);
    // 빈 조각("//")은 절대 경로로 해석되지 않는다.
    EXPECT_STATUS("//etc/passwd", 403);
    EXPECT_STATUS("/%2fetc%2fpasswd", 403);
    // 역슬래시, NUL, 제어 문자.
    EXPECT_STATUS("/%5c..%5coutside%5csecret.txt", 403);
    EXPECT_STATUS("/index.html%00.png", 403);
    EXPECT_STATUS("/index%0a.html", 403);
    EXPECT_STATUS("/index%7f.html", 403);
    // 잘못된 퍼센트 인코딩.
    EXPECT_STATUS("/%zz", 400);
    EXPECT_STATUS("/index.html%", 400);
    EXPECT_STATUS("/index.html%4", 400);
    // %XX로 쓴 일반 문자와 질의는 정상 경로다.
    EXPECT_STATUS("/%69ndex.html", 200);
    EXPECT_STATUS("/sub%2fpage.html", 200);
    EXPECT_STATUS("/index.html?file=../../etc/passwd", 200);
}

static void test_symlinks(void)
{
    // 경로의 어느 조각이든 심볼릭 링크면 따라가지 않는다. 루트 안을 가리켜도 같다.
    EXPECT_STATUS("/link-in.html", 403);
    EXPECT_STATUS("/link-out.txt", 403);
    EXPECT_STATUS("/link-dir/secret.txt", 403);
    EXPECT_STATUS("/link-dir", 403);
    EXPECT_STATUS("/link-dir/", 403);
}

// ---------------------------------------------------------------------------
// 없는 파일·권한 없음·디렉터리 응답 정책

static void test_missing_and_forbidden(void)
{
    EXPECT_STATUS("/no-such-file", 404);
    EXPECT_STATUS("/no-such-dir/index.html", 404);
    EXPECT_STATUS("/sub/no-such-file", 404);
    // 파일을 디렉터리처럼 쓰면 ENOTDIR이다.
    EXPECT_STATUS("/index.html/", 403);
    EXPECT_STATUS("/index.html/x", 403);
    // '/'로 끝나는 디렉터리 요청은 그 안의 index.html이다. 없으면 404다.
    EXPECT_STATUS("/", 200);
    EXPECT_STATUS("/sub/", 200);
    EXPECT_STATUS("/sub/page.html", 200);
    EXPECT_STATUS("/empty/", 404);
    // index.html이 디렉터리면 일반 파일이 아니므로 403이고, 다시 리디렉션하지 않는다.
    EXPECT_STATUS("/odd/", 403);
    // 디렉터리 안의 빈 조각은 여전히 403이다.
    EXPECT_STATUS("/sub//", 403);
    EXPECT_STATUS("/sub//page.html", 403);
    // FIFO는 O_NONBLOCK 덕분에 열 때 멈추지 않고, 일반 파일이 아니라서 거절된다.
    EXPECT_STATUS("/pipe", 403);
    if (geteuid() == 0) {
        // root는 권한 비트를 무시하므로 EACCES를 재현할 수 없다.
        printf("test_static: running as root, skipping the unreadable-file check\n");
    } else {
        EXPECT_STATUS("/locked.html", 403);
    }
}

// ---------------------------------------------------------------------------
// MIME과 본문

static void expect_file(const char *target, const char *type, const char *data, size_t length,
                        int line)
{
    struct reply reply = get(target);
    ++checks;
    bool ok = reply.result == 0 && reply.status == 200 && strcmp(reply.content_type, type) == 0 &&
              reply.content_length == (long long)length && reply.body_length == length &&
              memcmp(reply.body, data, length) == 0;
    if (!ok) {
        ++failures;
        fprintf(stderr, "test_static.c:%d: GET %s: status %d, type \"%s\", length %lld/%zu\n",
                line, target, reply.status, reply.content_type, reply.content_length,
                reply.body_length);
    }
}

#define EXPECT_FILE(target, type, literal) \
    expect_file((target), (type), (literal), sizeof(literal) - 1, __LINE__)

static void test_mime(void)
{
    EXPECT_FILE("/", "text/html; charset=utf-8", "<h1>index</h1>\n");
    EXPECT_FILE("/index.html", "text/html; charset=utf-8", "<h1>index</h1>\n");
    EXPECT_FILE("/style.css", "text/css; charset=utf-8", "body{}\n");
    EXPECT_FILE("/app.js", "text/javascript; charset=utf-8", "1;\n");
    EXPECT_FILE("/logo.png", "image/png", "\x89PNG\r\n\x1a\n\0png");
    EXPECT_FILE("/photo.JPG", "image/jpeg", "\xff\xd8\xff\0jpg");
    EXPECT_FILE("/photo.jpeg", "image/jpeg", "\xff\xd8\xff\0jpeg");
    EXPECT_FILE("/notes.txt", "application/octet-stream", "notes\n");
    EXPECT_FILE("/README", "application/octet-stream", "readme\n");
    EXPECT_FILE("/sub/page.html", "text/html; charset=utf-8", "<p>sub</p>\n");
    // 하위 디렉터리 index도 확장자는 index.html 기준이다.
    EXPECT_FILE("/sub/", "text/html; charset=utf-8", "<p>sub index</p>\n");
    EXPECT_FILE("/sub/?x=1", "text/html; charset=utf-8", "<p>sub index</p>\n");
}

// ---------------------------------------------------------------------------
// '/' 없이 디렉터리를 가리킨 요청의 301

static void expect_redirect(const char *method, const char *target, const char *location,
                            int line)
{
    static const char body[] = "Moved Permanently\n";
    struct reply reply = serve(method, target, READ_ALL, 0, CLIENT_WRITE_MS);
    bool head = strcmp(method, "HEAD") == 0;
    ++checks;
    bool ok = reply.result == 0 && reply.status == 301 && strcmp(reply.location, location) == 0 &&
              reply.content_length == (long long)(sizeof body - 1) &&
              reply.body_length == (head ? 0 : sizeof body - 1) &&
              (head || memcmp(reply.body, body, sizeof body - 1) == 0);
    if (!ok) {
        ++failures;
        fprintf(stderr, "test_static.c:%d: %s %s: status %d, Location \"%s\", length %lld/%zu\n",
                line, method, target, reply.status, reply.location, reply.content_length,
                reply.body_length);
    }
}

#define EXPECT_REDIRECT(target, location) expect_redirect("GET", (target), (location), __LINE__)

static void test_directory_redirect(void)
{
    EXPECT_REDIRECT("/sub", "/sub/");
    EXPECT_REDIRECT("/empty", "/empty/");
    EXPECT_REDIRECT("/odd", "/odd/");
    // 질의와 %XX는 요청에 온 그대로 둔다.
    EXPECT_REDIRECT("/sub?x=1&y=/", "/sub/?x=1&y=/");
    EXPECT_REDIRECT("/%73ub", "/%73ub/");
    expect_redirect("HEAD", "/sub", "/sub/", __LINE__);
    // 리디렉션은 실제로 연 디렉터리에만 준다. 링크·빈 조각·루트 밖은 그 전에 거절되므로
    // Location이 "//host"(다른 사이트로 가는 주소)가 되는 일이 없다.
    EXPECT_STATUS("/link-dir", 403);
    EXPECT_STATUS("//sub", 403);
    EXPECT_STATUS("/%2fsub", 403);
    EXPECT_STATUS("//evil.example", 403);
    EXPECT_STATUS("/%2f%2fevil.example", 403);
    EXPECT_STATUS("/sub/..", 403);
    EXPECT_STATUS("/no-such-dir", 404);
}

// ---------------------------------------------------------------------------
// 큰 파일: 반복 write의 길이 정확성, 조각별 기한, 중도 종료 시 FD 회수

static bool big_body_ok(const struct reply *reply)
{
    return reply->result == 0 && reply->status == 200 && reply->content_length == BIG_SIZE &&
           reply->body_length == BIG_SIZE && memcmp(reply->body, big_data, BIG_SIZE) == 0;
}

static void test_big_file(void)
{
    struct reply reply = get("/big.bin");
    CHECK(big_body_ok(&reply));
    CHECK(strcmp(reply.content_type, "application/octet-stream") == 0);

    // HEAD는 GET과 같은 Content-Length를 주고 본문은 보내지 않는다.
    reply = serve("HEAD", "/big.bin", READ_ALL, 0, CLIENT_WRITE_MS);
    CHECK(reply.result == 0 && reply.status == 200);
    CHECK(reply.content_length == BIG_SIZE && reply.body_length == 0);

    // 응답 헤더 기한(100ms)이 지난 뒤에 읽기 시작해도 본문은 끝까지 간다.
    // 응답 전체에 기한 하나를 걸면 여기서 소켓 버퍼가 찬 채 기한을 넘겨 끊긴다.
    reply = serve("GET", "/big.bin", READ_LATE, 400, 100);
    CHECK(big_body_ok(&reply));

    // 전혀 읽지 않는 클라이언트는 조각 하나의 기한(CLIENT_WRITE_MS) 뒤에 끊는다.
    reply = serve("GET", "/big.bin", READ_NOTHING, 0, CLIENT_WRITE_MS);
    CHECK(reply.result < 0);
    CHECK(reply.elapsed_ms >= CLIENT_WRITE_MS - 50);
    CHECK(reply.elapsed_ms < CLIENT_WRITE_MS + 1500);
}

static void test_client_abort(void)
{
    int before = open_fd_count();
    CHECK(before > 0);
    // 헤더와 본문 일부만 읽고 끊으면 send가 EPIPE로 실패하고, 기한을 기다리지 않고 돌아온다.
    for (int i = 0; i < 20; ++i) {
        struct reply reply = serve("GET", "/big.bin", READ_THEN_QUIT, HEAD_ROOM + 65536,
                                   CLIENT_WRITE_MS);
        CHECK(reply.result < 0);
        CHECK(reply.elapsed_ms < CLIENT_WRITE_MS);
    }
    // 오류 응답을 보내는 경로도 FD를 남기지 않는다.
    for (int i = 0; i < 20; ++i) {
        (void)get("/no-such-dir/index.html");
        (void)get("/sub/page.html/");
        (void)get("/sub");
        (void)get("/sub/");
        (void)get("/odd/");
        (void)get("/empty/");
    }
    CHECK(open_fd_count() == before);
}

int main(void)
{
    // 서버 프로세스와 같이, 닫힌 소켓에 쓰면 SIGPIPE 대신 EPIPE를 받는다.
    signal(SIGPIPE, SIG_IGN);
    reply_capacity = BIG_SIZE + HEAD_ROOM;
    reply_bytes = malloc(reply_capacity);
    if (reply_bytes == NULL) {
        fail_setup("malloc");
    }
    build_root();
    test_root_boundary();
    test_symlinks();
    test_missing_and_forbidden();
    test_mime();
    test_directory_redirect();
    test_big_file();
    test_client_abort();
    remove_root();
    free(reply_bytes);
    printf("test_static: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
