#include "http.h"
#include "proxy.h"
#include "tcp.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool resolve_backend(const char *host, uint16_t port, struct sockaddr_in *address);

static bool add_backend(struct backend_pool *pool, const char *endpoint)
{
    const char *colon = strchr(endpoint, ':');
    char host[256];
    uint16_t port;
    if (pool->count == BACKEND_LIMIT || colon == NULL || colon == endpoint ||
        (size_t)(colon - endpoint) >= sizeof host || !parse_port(colon + 1, &port)) {
        return false;
    }
    size_t length = (size_t)(colon - endpoint);
    memcpy(host, endpoint, length);
    host[length] = '\0';
    if (!resolve_backend(host, port, &pool->addresses[pool->count])) {
        return false;
    }
    ++pool->count;
    return true;
}

static bool valid_route_prefix(const char *prefix)
{
    size_t length = strlen(prefix);
    return length > 0 && length <= HTTP_TARGET_LIMIT && prefix[0] == '/' &&
           prefix[length - 1] == '/' && strpbrk(prefix, "?# \t\r\n") == NULL;
}

static bool parse_options(int argc, char **argv, uint16_t *port,
                          const char **static_root, const char **backend_host,
                          uint16_t *backend_port, struct server_config *config)
{
    bool single_backend = false;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc) {
            return false;
        }
        const char *option = argv[i];
        const char *value = argv[i + 1];
        if (strcmp(option, "--port") == 0) {
            if (!parse_port(value, port)) {
                return false;
            }
        } else if (strcmp(option, "--static-root") == 0) {
            *static_root = value;
        } else if (strcmp(option, "--route-prefix") == 0 && valid_route_prefix(value)) {
            config->route_prefix = value;
        } else if (strcmp(option, "--backend-address") == 0) {
            *backend_host = value;
            single_backend = true;
        } else if (strcmp(option, "--backend-port") == 0) {
            if (!parse_port(value, backend_port)) {
                return false;
            }
            single_backend = true;
        } else if (strcmp(option, "--backend") == 0) {
            if (!add_backend(config->backends, value)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return !single_backend || config->backends->count == 0;
}

static bool resolve_backend(const char *host, uint16_t port, struct sockaddr_in *address)
{
    char service[sizeof "65535"];
    snprintf(service, sizeof service, "%u", (unsigned)port);

    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
        .ai_flags = AI_NUMERICSERV
    };
    struct addrinfo *result;
    int error = getaddrinfo(host, service, &hints, &result);
    if (error != 0) {
        fprintf(stderr, "resolve backend %s:%s: %s\n", host, service, gai_strerror(error));
        return false;
    }

    memcpy(address, result->ai_addr, sizeof *address);
    freeaddrinfo(result);
    return true;
}

static void handle_client(int client_fd, int64_t read_deadline, const void *context)
{
    const struct server_config *config = context;
    struct request request;
    int status = read_request(client_fd, &request, read_deadline);
    if (status == 200) {
        (void)route_request(client_fd, &request, config);
    } else if (status != 0) {
        (void)send_error_for(client_fd, &request, status, NULL, deadline_after(CLIENT_WRITE_MS));
    }
}

int main(int argc, char **argv)
{
    uint16_t port = 8080;
    uint16_t backend_port = 8001;
    const char *static_root = "/app/public";
    const char *backend_host = "127.0.0.1";
    struct backend_pool backends = {0};
    atomic_init(&backends.next, 0);
    struct server_config config = {
        .route_prefix = "/api/",
        .backends = &backends
    };
    if (!parse_options(argc, argv, &port, &static_root, &backend_host, &backend_port, &config)) {
        fprintf(stderr, "Usage: %s [--port PORT] [--static-root DIR] [--route-prefix PATH/] "
                        "[[--backend-address HOST] [--backend-port PORT] | "
                        "--backend HOST:PORT [--backend HOST:PORT ...]]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (backends.count == 0) {
        if (!resolve_backend(backend_host, backend_port, &backends.addresses[0])) {
            return EXIT_FAILURE;
        }
        backends.count = 1;
    }
    config.static_root_fd = open(static_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (config.static_root_fd < 0) {
        perror("open static root");
        return EXIT_FAILURE;
    }
    int result = serve_tcp("front", htonl(INADDR_ANY), port, handle_client, &config);
    close(config.static_root_fd);
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
