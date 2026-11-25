#ifndef PROXY_H
#define PROXY_H

#include "http.h"
#include <netinet/in.h>
#include <stdatomic.h>

enum { BACKEND_LIMIT = 16 };

struct backend_pool {
    struct sockaddr_in addresses[BACKEND_LIMIT];
    size_t count;
    atomic_size_t next;
};

struct server_config {
    int static_root_fd;
    const char *route_prefix;
    struct backend_pool *backends;
};

int route_request(int client_fd, const struct request *request, const struct server_config *config);

#endif
