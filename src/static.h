#ifndef STATIC_H
#define STATIC_H

#include "http.h"

int serve_static(int client_fd, int root_fd, const struct request *request, int64_t deadline);

#endif
