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


REQUEST_LINE_LIMIT = 8000
HEADER_LIMIT = 8192
REQUEST_HEAD_LIMIT = REQUEST_LINE_LIMIT + 2 + HEADER_LIMIT
HOST = b"Host: localhost\r\n"


def exchange_raw(wire):
    """Send raw bytes and read until the server closes, returning (status, headers, body)."""
    with socket.create_connection(FRONT, timeout=5) as sock:
        sock.sendall(wire)
        received = bytearray()
        while chunk := sock.recv(65536):
            received.extend(chunk)
    head, _, body = bytes(received).partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    headers = {}
    for line in lines[1:]:
        name, _, value = line.partition(":")
        headers[name.strip().lower()] = value.strip()
    return int(lines[0].split(" ")[1]), headers, body


def status_of(wire):
    return exchange_raw(wire)[0]


def get(target, fields=HOST, version=b"HTTP/1.1", method=b"GET"):
    return method + b" " + target + b" " + version + b"\r\n" + fields + b"\r\n"


def fill_head_limit(prefix):
    # Exactly the buffer size, so the server reads every byte and closes without a reset.
    assert len(prefix) < REQUEST_HEAD_LIMIT
    return prefix + b"a" * (REQUEST_HEAD_LIMIT - len(prefix))


def line_of(length):
    # A query keeps the backend answering 200 however long the target is.
    prefix = f"{API}?pad=".encode("ascii")
    pad = length - len(b"GET  HTTP/1.1") - len(prefix)
    return prefix + b"a" * pad


def fields_of(length):
    # Field section as counted by the server: field lines plus the final empty line.
    fixed = HOST + b"X-Pad: " + b"\r\n" + b"\r\n"
    return HOST + b"X-Pad: " + b"a" * (length - len(fixed)) + b"\r\n"


def check_limits():
    # RFC 9110 4.1, RFC 9112 3: request lines of 8000 octets are supported; longer ones get
    # 414, or 501 when the method is longer than any implemented one.
    assert status_of(get(line_of(REQUEST_LINE_LIMIT))) == 200
    assert status_of(get(line_of(REQUEST_LINE_LIMIT + 1))) == 414
    assert status_of(fill_head_limit(b"GET /")) == 414
    assert status_of(b"X" * 9000 + b" / HTTP/1.1\r\n" + HOST + b"\r\n") == 501
    assert status_of(fill_head_limit(b"X")) == 501
    # The 8 KiB field section limit is separate from the request line.
    assert status_of(get(b"/", fields_of(HEADER_LIMIT))) == 200
    assert status_of(get(b"/", fields_of(HEADER_LIMIT + 1))) == 431
    assert status_of(fill_head_limit(b"GET / HTTP/1.1\r\n" + HOST + b"X-Pad: ")) == 431
    print("Request line 8000: 200, longer: 414 (long method: 501); header fields over 8 KiB: 431")


def check_request_line():
    # RFC 9110 9.1: unimplemented methods get 501; known ones not allowed here get 405.
    for method in (b"PUT", b"DELETE", b"get"):
        assert status_of(get(b"/", method=method)) == 501, method
        assert status_of(get(API.encode(), method=method)) == 501, method
    status, headers, _ = exchange_raw(
        b"POST / HTTP/1.1\r\n" + HOST + b"Content-Length: 0\r\n\r\n")
    assert status == 405 and headers["allow"] == "GET, HEAD", headers
    # RFC 9110 2.5: any HTTP/1.x is served; RFC 9112 3.2: only HTTP/1.1 requires Host.
    assert status_of(get(b"/", b"", b"HTTP/1.0")) == 200
    assert status_of(get(b"/", HOST, b"HTTP/1.2")) == 200
    assert status_of(get(b"/", HOST, b"HTTP/2.0")) == 505
    assert status_of(get(b"/", HOST, b"HTTP/0.9")) == 505
    assert status_of(get(b"/", b"Host: bad host\r\n", b"HTTP/1.0")) == 400
    # RFC 9112 6.1: Transfer-Encoding in HTTP/1.0 is faulty framing; in HTTP/1.1 it is 501.
    chunked = HOST + b"Transfer-Encoding: chunked\r\n"
    assert status_of(get(API.encode(), chunked, b"HTTP/1.0", b"POST")) == 400
    assert status_of(get(API.encode(), chunked, b"HTTP/1.1", b"POST")) == 501
    # RFC 9112 2.2: empty lines before the request line are ignored.
    assert status_of(b"\r\n\r\n" + get(b"/style.css")) == 200
    print("Unimplemented methods: 501; HTTP/1.0 and 1.2: 200; HTTP/2.0: 505; leading CRLF ignored")


def check_uri_grammar():
    # RFC 9112 3.2.1 with RFC 3986 3.3-3.4: only pchar, "/", "?" and valid %XX are allowed.
    for target in (b"/style.css?x=a/b?c:@!$&'()*+,;=", b"/%73tyle.css"):
        assert status_of(get(target)) == 200, target
    for target in (b'/a"b', b"/a<b", b"/a{b}", b"/a|b", b"/a^b", b"/a`b", b"/a\\b",
                   b"/%zz", b"/%4", b"/style.css%"):
        assert status_of(get(target)) == 400, target
    # RFC 9110 7.2 with RFC 3986 3.2.2-3.2.3: Host is uri-host [ ":" port ].
    for host in (b"localhost:8080", b"127.0.0.1", b"[::1]:8080", b"localhost:", b"%6Cocalhost"):
        assert status_of(get(b"/", b"Host: " + host + b"\r\n")) == 200, host
    for host in (b"a:b:c", b"localhost:abc", b"[::1", b"[zz]", b"local%zzhost", b"host/x",
                 b"user@host", "café".encode("utf-8"), b":8080"):
        assert status_of(get(b"/", b"Host: " + host + b"\r\n")) == 400, host
    # RFC 9112 3.2.2: absolute-form is accepted, and its authority replaces Host.
    for target, expected_media in (
        (b"http://localhost:8080/style.css", "text/css; charset=utf-8"),
        (b"HTTP://localhost:8080", "text/html; charset=utf-8"),
        (b"http://localhost:8080?x=1", "text/html; charset=utf-8"),
        (b"http://[::1]:8080/style.css", "text/css; charset=utf-8"),
        (b"http://localhost:8080/api/messages", "application/json; charset=utf-8"),
    ):
        status, headers, body = exchange_raw(get(target, b"Host: elsewhere.example\r\n"))
        assert status == 200 and headers["content-type"] == expected_media and body, target
    for target in (b"ftp://localhost/", b"http:///style.css", b"http://user@localhost/",
                   b"http://localhost:8080/a%zz", b"*"):
        assert status_of(get(target)) == 400, target
    print("Target and Host grammar: valid 200, invalid 400; absolute-form: 200")


def check_head():
    # RFC 9110 9.3.2: HEAD has GET's header fields but never content, including on errors.
    for path, expected in (("/", 200), ("/style.css", 200), (API, 200), ("/no-such-file", 404)):
        get_status, get_headers, get_body = exchange_raw(get(path.encode("ascii")))
        status, headers, body = exchange_raw(get(path.encode("ascii"), method=b"HEAD"))
        assert (status, body) == (expected, b""), (path, status, body)
        assert get_status == expected and headers["content-type"] == get_headers["content-type"]
        if path != API:  # The message list may change between the two requests.
            assert headers["content-length"] == str(len(get_body)), path
    status, headers, body = exchange_raw(b"HEAD / HTTP/1.1\r\n\r\n")
    assert (status, body) == (400, b"") and headers["content-length"] != "0"
    print("HEAD: GET's status and headers with no content")


def post_expecting(version, expect, body):
    head = (f"POST {API} {version}\r\nHost: localhost\r\nExpect: {expect}\r\n"
            f"Content-Type: {PLAIN['Content-Type']}\r\nContent-Length: {len(body)}\r\n\r\n")
    sock = socket.create_connection(FRONT, timeout=5)
    sock.sendall(head.encode("ascii"))
    return sock


def check_expect():
    # RFC 9110 10.1.1: an HTTP/1.1 100-continue gets 100 before the content is sent.
    body = f"continue {uuid.uuid4().hex[:8]}".encode("ascii")
    with post_expecting("HTTP/1.1", "100-Continue", body) as sock:
        interim = bytearray()
        while not interim.endswith(b"\r\n\r\n"):
            chunk = sock.recv(1)
            assert chunk, "connection closed before 100 Continue"
            interim.extend(chunk)
        assert interim == b"HTTP/1.1 100 Continue\r\n\r\n", interim
        sock.sendall(body)
        assert read_raw(sock)[0] == 201
    # HTTP/1.0 MUST ignore 100-continue, so nothing arrives until the content does.
    body = f"legacy {uuid.uuid4().hex[:8]}".encode("ascii")
    with post_expecting("HTTP/1.0", "100-continue", body) as sock:
        assert not select.select([sock], [], [], 0.2)[0], "unexpected interim response"
        sock.sendall(body)
        assert read_raw(sock)[0] == 201
    with post_expecting("HTTP/1.1", "something-else", b"x") as sock:
        assert read_raw(sock)[0] == 417
    print("Expect 100-continue: 100 then 201 (ignored in HTTP/1.0); other expectations: 417")


def check_http_boundaries():
    check_limits()
    check_request_line()
    check_uri_grammar()
    check_head()
    check_expect()


def check_normal():
    wait_for_backend()
    check_static()
    check_http_boundaries()
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
