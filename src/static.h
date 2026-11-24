#ifndef STATIC_H
#define STATIC_H

#include "http.h"

// root_fd 아래의 파일 하나를 응답한다. deadline은 응답 헤더(또는 오류 응답)까지의 기한이고,
// 본문은 16KB 조각마다 CLIENT_WRITE_MS의 기한을 새로 받는다. 전송이 끊기면 -1이다.
int serve_static(int client_fd, int root_fd, const struct request *request, int64_t deadline);

#endif
