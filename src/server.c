#include "http.h"
#include "proxy.h"
#include "tcp.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool valid_route_prefix(const char *prefix)
{
    size_t length = strlen(prefix);
    return length > 0 && length <= HTTP_TARGET_LIMIT && prefix[0] == '/' &&
           prefix[length - 1] == '/' && strpbrk(prefix, "?# \t\r\n") == NULL;
}

static bool parse_options(int argc, char **argv, uint16_t *port,
                          const char **static_root, struct server_config *config)
{
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
            if (inet_pton(AF_INET, value, &config->backend.sin_addr) != 1) {
                return false;
            }
        } else if (strcmp(option, "--backend-port") == 0) {
            uint16_t backend_port;
            if (!parse_port(value, &backend_port)) {
                return false;
            }
            config->backend.sin_port = htons(backend_port);
        } else {
            return false;
        }
    }
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
        (void)send_error(client_fd, status, NULL, deadline_after(CLIENT_WRITE_MS));
    }
}

int main(int argc, char **argv)
{
    uint16_t port = 8080;
    const char *static_root = "/app/public";
    struct server_config config = {
        .route_prefix = "/api/",
        .backend = {.sin_family = AF_INET, .sin_port = htons(8001),
                    .sin_addr.s_addr = htonl(INADDR_LOOPBACK)}
    };
    if (!parse_options(argc, argv, &port, &static_root, &config)) {
        fprintf(stderr, "Usage: %s [--port PORT] [--static-root DIR] [--route-prefix PATH/] "
                        "[--backend-address IPV4] [--backend-port PORT]\n", argv[0]);
        return EXIT_FAILURE;
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
