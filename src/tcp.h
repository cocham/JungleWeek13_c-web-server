#ifndef TCP_H
#define TCP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

enum { CLIENT_READ_MS = 2000, CLIENT_WRITE_MS = 2000 };

typedef void (*connection_handler)(int client_fd, int64_t read_deadline, const void *context);

bool parse_port(const char *text, uint16_t *port);
int set_nonblocking(int fd);
int64_t monotonic_ms(void);
int64_t deadline_after(int milliseconds);
int wait_for_io(int fd, short events, int64_t deadline);
ssize_t receive_bytes(int fd, char *buffer, size_t capacity, int64_t deadline);
int send_all(int fd, const char *bytes, size_t length, int64_t deadline);

// The pool owns all accepted descriptors. The handler borrows one for the call.
int serve_tcp(const char *name, uint32_t bind_address, uint16_t port,
              connection_handler handler, const void *context);

#endif
