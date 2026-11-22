#ifndef PROXY_H
#define PROXY_H

#include "http.h"
#include <netinet/in.h>

struct server_config {
    int static_root_fd;
    const char *route_prefix;
    struct sockaddr_in backend;
};

int route_request(int client_fd, const struct request *request, const struct server_config *config);

#endif
