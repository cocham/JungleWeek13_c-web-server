"""Open N connections to the front server at the same time and keep each request
unfinished for HOLD seconds, so every worker that picks one up stays busy.

Passes only when every later client gets its 200 response before client1 does,
which cannot happen if a single worker handles the connections one by one.

usage: python two_clients.py [N] [HOLD]
"""
import socket
import sys
import threading
import time

HOST, PORT = "127.0.0.1", 8080
CLIENT_READ_S = 2.0  # CLIENT_READ_MS in src/tcp.h
N = int(sys.argv[1]) if len(sys.argv) > 1 else 2
HOLD = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
if N < 2 or not 0 < HOLD < CLIENT_READ_S:
    sys.exit(f"usage: {sys.argv[0]} [N >= 2] [0 < HOLD < {CLIENT_READ_S}]")

start = time.monotonic()
barrier = threading.Barrier(N)
lock = threading.Lock()
statuses = {}  # client index -> status line
finished = {}  # client index -> seconds since start when the response ended
errors = {}    # client index -> exception raised in that thread


def log(name, text):
    with lock:
        print(f"{time.monotonic() - start:6.3f}s  {name}: {text}", flush=True)


def client(index):
    name = f"client{index}"
    # client1 holds longest, the last client shortest: with one worker the short
    # ones would have to wait behind client1; with several they finish first.
    hold = HOLD * (N - index + 1) / N
    barrier.wait()
    time.sleep(0.05 * (index - 1))  # connect in order, so client1 is accepted first
    with socket.create_connection((HOST, PORT), timeout=5) as sock:
        log(name, f"connected (local port {sock.getsockname()[1]})")
        sock.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n")  # header not finished yet
        log(name, f"sent half of the header, holding {hold:.2f}s")
        time.sleep(hold)
        sock.sendall(b"\r\n")  # blank line: request is now complete
        log(name, "sent the blank line")
        response = b""
        while chunk := sock.recv(4096):
            response += chunk
        status = response.split(b"\r\n", 1)[0].decode()
        finished[index] = time.monotonic() - start
        statuses[index] = status
        log(name, f"got response: {status}")


def run(index):
    try:
        client(index)
    except Exception as error:  # report it from the main thread instead of exiting 0
        errors[index] = error
        log(f"client{index}", f"failed: {error!r}")


threads = [threading.Thread(target=run, args=(i + 1,)) for i in range(N)]
for thread in threads:
    thread.start()
for thread in threads:
    thread.join()

assert not errors, f"client errors: {errors}"
assert all(statuses[i] == "HTTP/1.1 200 OK" for i in statuses), statuses
late = [i for i in range(2, N + 1) if finished[i] >= finished[1]]
assert not late, (
    f"client(s) {late} finished after client1 ({finished}); "
    "the connections were not handled at the same time"
)
log("all", f"PASS: clients 2..{N} finished before client1, so they ran on separate workers")
