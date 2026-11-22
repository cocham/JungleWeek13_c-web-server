CC ?= cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -std=c17 -O2 -Wall -Wextra -Wpedantic -Werror
THREAD_FLAGS = -pthread

COMMON_OBJECTS = build/tcp.o build/http.o
FRONT_OBJECTS = build/server.o build/static.o build/proxy.o
BACKEND_OBJECTS = build/backend.o
OBJECTS = $(COMMON_OBJECTS) $(FRONT_OBJECTS) $(BACKEND_OBJECTS)
PROGRAMS = build/guestbook build/guestbook-backend

.PHONY: all clean test
all: $(PROGRAMS)

build/guestbook: $(FRONT_OBJECTS) $(COMMON_OBJECTS)
	$(CC) $(LDFLAGS) $(THREAD_FLAGS) $^ $(LDLIBS) -o $@

build/guestbook-backend: $(BACKEND_OBJECTS) $(COMMON_OBJECTS)
	$(CC) $(LDFLAGS) $(THREAD_FLAGS) $^ $(LDLIBS) -o $@

build/%.o: src/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -MMD -MP -c $< -o $@

build:
	mkdir -p $@

test: all
	PATH="$(CURDIR)/build:$$PATH" GUESTBOOK_STATIC_ROOT="$(CURDIR)/public" \
		timeout -k 2 20 bash tests/shutdown.sh

clean:
	rm -f $(PROGRAMS) $(OBJECTS) $(OBJECTS:.o=.d)

-include $(OBJECTS:.o=.d)
