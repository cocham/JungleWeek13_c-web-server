CC ?= cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -std=c17 -O2 -Wall -Wextra -Wpedantic -Werror
THREAD_FLAGS = -pthread

COMMON_OBJECTS = build/tcp.o build/http.o
FRONT_OBJECTS = build/server.o build/static.o build/proxy.o
BACKEND_OBJECTS = build/backend.o
OBJECTS = $(COMMON_OBJECTS) $(FRONT_OBJECTS) $(BACKEND_OBJECTS)
PROGRAMS = build/guestbook build/guestbook-backend
TEST_OBJECTS = build/test_http.o build/test_static.o
TEST_PROGRAMS = build/test_http build/test_static

.PHONY: all clean test unit
all: $(PROGRAMS)

build/guestbook: $(FRONT_OBJECTS) $(COMMON_OBJECTS)
	$(CC) $(LDFLAGS) $(THREAD_FLAGS) $^ $(LDLIBS) -o $@

build/guestbook-backend: $(BACKEND_OBJECTS) $(COMMON_OBJECTS)
	$(CC) $(LDFLAGS) $(THREAD_FLAGS) $^ $(LDLIBS) -o $@

build/test_http: build/test_http.o $(COMMON_OBJECTS)
	$(CC) $(LDFLAGS) $(THREAD_FLAGS) $^ $(LDLIBS) -o $@

build/test_static: build/test_static.o build/static.o $(COMMON_OBJECTS)
	$(CC) $(LDFLAGS) $(THREAD_FLAGS) $^ $(LDLIBS) -o $@

build/%.o: src/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -MMD -MP -c $< -o $@

build/test_%.o: tests/test_%.c | build
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) $(THREAD_FLAGS) -MMD -MP -c $< -o $@

build:
	mkdir -p $@

unit: $(TEST_PROGRAMS)
	./build/test_http
	./build/test_static

test: all unit
	PATH="$(CURDIR)/build:$$PATH" GUESTBOOK_STATIC_ROOT="$(CURDIR)/public" \
		timeout -k 2 20 bash tests/shutdown.sh

clean:
	rm -f $(PROGRAMS) $(TEST_PROGRAMS) $(OBJECTS) $(TEST_OBJECTS) \
		$(OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d)

-include $(OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d)
