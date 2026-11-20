import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import closing
import http.client
import json
import select
import socket
from threading import Event
import time
import uuid


FRONT = ("127.0.0.1", 8080)
UPSTREAM = ("127.0.0.1", 8001)
API = "/api/messages"
PLAIN = {"Content-Type": "text/plain; charset=utf-8"}


def read_http(response):
    data = response.read()
    declared = response.getheader("Content-Length")
    assert declared == str(len(data)), (declared, len(data))
    assert response.getheader("Connection") == "close"
    return response.status, response.getheader("Content-Type"), data


def request(method, path, body=None, headers=None):
    with closing(http.client.HTTPConnection(*FRONT, timeout=5)) as connection:
        connection.request(method, path, body=body, headers=headers or {})
        with closing(connection.getresponse()) as response:
            return read_http(response)


def expect_status(expected, method, path, body=None, headers=None):
    status, content_type, data = request(method, path, body, headers)
    assert status == expected, f"{method} {path}: expected {expected}, got {status}"
    return content_type, data


def read_raw(sock):
    with closing(http.client.HTTPResponse(sock)) as response:
        response.begin()
        return read_http(response)


def post_in_pieces(text):
    body = text.encode("utf-8")
    head = (
        f"POST {API} HTTP/1.1\r\nHost: localhost\r\n"
        f"Content-Type: {PLAIN['Content-Type']}\r\nContent-Length: {len(body)}\r\n\r\n"
    ).encode("ascii")
    with socket.create_connection(FRONT, timeout=5) as sock:
        sock.sendall(head[:-1])
        assert not select.select([sock], [], [], 0.1)[0], "response before the full header"
        sock.sendall(head[-1:] + body[:2])
        assert not select.select([sock], [], [], 0.1)[0], "response before the full body"
        sock.sendall(body[2:])
        status, media, data = read_raw(sock)
    assert status == 201, (status, data)
    assert media == "application/json; charset=utf-8", media
    assert json.loads(data) == {"message": text}


def wait_for_backend():
    deadline = time.monotonic() + 7
    while time.monotonic() < deadline:
        try:
            status, _, _ = request("GET", API)
            if status == 200:
                return
        except OSError:
            pass
        time.sleep(0.15)
    raise AssertionError("guestbook API did not become ready")


def check_static():
    for path, expected in (
        ("/", "text/html; charset=utf-8"),
        ("/style.css", "text/css; charset=utf-8"),
        ("/app.js", "text/javascript; charset=utf-8"),
    ):
        media, body = expect_status(200, "GET", path)
        assert media == expected and body, (path, media)
    expect_status(404, "GET", "/no-such-file")
    print("Static HTML/CSS/JS and MIME: 200; missing file: 404", flush=True)


def check_normal():
    wait_for_backend()
    check_static()
    media, before = expect_status(200, "GET", API)
    assert media == "application/json; charset=utf-8", media
    before = json.loads(before)["messages"]

    text = f'한 줄 검증 "{uuid.uuid4().hex[:10]}" \\ <글>'
    post_in_pieces(text)
    _, after = expect_status(200, "GET", API)
    assert json.loads(after)["messages"] == [*before, text]
    print("Split POST: 201 with exact UTF-8 body; subsequent GET includes new message")

    expect_status(400, "POST", API, b"two\nlines", PLAIN)
    expect_status(400, "POST", API, b"\xff", PLAIN)
    expect_status(413, "POST", API, "가".encode("utf-8") * 81, PLAIN)
    expect_status(415, "POST", API, b"{}", {"Content-Type": "application/json"})
    with socket.create_connection(FRONT, timeout=5) as sock:
        sock.sendall(b"POST /api/messages HTTP/1.1\r\nHost: localhost\r\n\r\n")
        assert read_raw(sock)[0] == 411
    print("Invalid message checks: 400, 411, 413, 415")

    paths = ("/", API) * 8
    with ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(lambda path: request("GET", path)[0], paths))
    assert results == [200] * len(paths), results
    print(f"Concurrent GETs: {len(paths)}/{len(paths)} returned 200")


def serve_one_response(listener, wire, received):
    client, _ = listener.accept()
    with client:
        client.settimeout(5)
        incoming = bytearray()
        while b"\r\n\r\n" not in incoming:
            chunk = client.recv(4096)
            assert chunk and len(incoming) + len(chunk) <= 8192
            incoming.extend(chunk)
        received.set()
        if wire is None:
            # Keep the connection open until the front server's read deadline expires.
            while client.recv(4096):
                pass
        else:
            client.sendall(wire)


def check_fake_upstream(expected, wire):
    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(UPSTREAM)
        listener.listen(1)
        listener.settimeout(6)
        received = Event()
        with ThreadPoolExecutor(max_workers=2) as pool:
            server = pool.submit(serve_one_response, listener, wire, received)
            api = pool.submit(expect_status, expected, "GET", API)
            assert received.wait(timeout=5), "proxy did not send an upstream request"
            if wire is None:
                expect_status(200, "GET", "/")
                assert not api.done(), "expected the upstream request to still be pending"
            api.result(timeout=6)
            server.result(timeout=6)
    expect_status(200, "GET", "/")


def check_upstream_failure():
    expect_status(502, "GET", API)
    expect_status(200, "GET", "/")
    print("Stopped backend: 502; static page: 200", flush=True)
    check_fake_upstream(504, None)
    print("Nonresponding backend: 504; static page during and after: 200", flush=True)
    check_fake_upstream(502, b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\nConnection: close\r\n\r\nok")
    print("Truncated backend response: 502; static page: 200", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Run guestbook checks in the Compose network")
    parser.add_argument("mode", choices=("normal", "upstream-failure"))
    args = parser.parse_args()
    if args.mode == "normal":
        check_normal()
    else:
        check_upstream_failure()
