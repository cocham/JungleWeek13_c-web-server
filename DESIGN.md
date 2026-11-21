# 한 줄 방명록 설계

과제 원문과 공통 기준 구현 이슈 #2에 따라 범위를 정했다.

## 과제 범위

| 구분 | 포함 내용 |
| --- | --- |
| 기준 버전에 구현할 것 | C·표준 라이브러리·POSIX 소켓만 사용. TCP 수락·송수신, HTTP/1.1 요청 줄·헤더·GET/POST 본문과 응답, 지정 루트의 HTML/CSS/JS/PNG/JPEG MIME 및 403/404/500, pthread 풀, 경로 기반 리버스 프록시와 별도 C 백엔드 한 대, CLI 설정, 메모리 방명록, Makefile·Docker·한국어 README, 시그널 종료·자원 회수. Docker·curl·브라우저로 정적 파일, POST, 동시 요청, 단절·타임아웃을 확인한다. |
| 이후 팀이 완성할 것 | 과제 원문의 다중 백엔드 라우팅 요구, HTTP 파서·MIME·라우팅/프록시 단위 테스트와 엣지 케이스 확대, `ab`/`wrk`/`hey` 처리량 측정과 C10K 토론, Valgrind 또는 AddressSanitizer/LeakSanitizer 검사, macOS 등 이식성 확인. 대표가 팀 저장소를 만들고 팀원을 초대한다. 주간 학습 루틴과 그라운드 룰을 확인하고, 목요일 13시 발표에서 대표 1명이 README로 데모와 검증을 설명한다(발표 4분, Q&A 1분). |
| 선택 기능 | Round-Robin 등 로드 밸런싱, Host 기반 라우팅, Forward Proxy 및 캐시, CGI, keep-alive, 추가 OS 최적화. 과제의 동시성 선택지 중 `fork()` 방식과 전체 I/O 이벤트 루프도 확장 후보로 둔다. |

1턴은 Docker에서 `GET /`에 고정 텍스트를 보낸다. 2턴에 모듈 인터페이스, 작업 큐, 정적 화면과 실제 백엔드 조회를 연결한다. 3턴에 등록, 502/504, Makefile과 실행·테스트 문서를 붙인다. 4턴에 Docker와 실제 브라우저로 동작을 확인하고 역할별 후속 작업을 정리한다.

## 등록 요청 한 건의 흐름

등록 버튼은 아래 경로로 POST를 보내고, 성공 뒤 같은 API에 GET을 보낸다. 실제 실행 명령은 `README.md`에 있다.

1. 브라우저의 등록 버튼이 `fetch('/api/messages', { method: 'POST', ... })`를 호출한다. TCP 3-way handshake는 커널이 처리한다. `open_listener()`의 `socket → setsockopt(SO_REUSEADDR) → bind → listen`을 거쳐 `accept_clients()`의 `accept()`가 **클라이언트 FD**를 얻는다. `bind()` 실패는 오류를 기록하고 종료한다.
2. `accept_clients()`가 클라이언트 FD와 수락 시각에서 계산한 읽기 기한을 제한된 FIFO 작업 큐에 넣는다. pthread의 `worker_main()`이 꺼내 `handle_client()`에 전달한다. 작업자는 한 연결을 순서대로 처리하고, `poll()` 및 논블로킹 I/O로 각 단계의 대기 한도를 적용한다.
3. `read_request()`가 요청 줄, 헤더, 본문을 구분한다. 나눠 들어오는 `\r\n\r\n`을 찾아 그 뒤 이미 받은 바이트를 유지하고 `Content-Length`에 맞춰 남은 바이트를 읽는다. 요청 저장 공간은 작업자 소유이며 필드를 가리키는 포인터는 해당 요청을 처리하는 동안 유효하다.
4. `route_request()`는 질의 문자열을 제외한 경로로 분기한다. 정적 GET이면 `serve_static()`이 `/`를 `index.html`에 매핑해 지정 루트에서 파일을 열고 MIME을 정한다. API 경로면 `proxy_request()`가 별도의 **백엔드 FD**를 `socket()`으로 열고 설정된 주소에 `connect()`한다. 검증한 메서드·대상·본문을 보내고 `Host`, `Content-Length`, `Connection`을 다시 만든다. 기준 API에 필요한 `Content-Type`만 전달하고 연결별 헤더는 전달하지 않는다.
5. 백엔드 `handle_messages()`가 같은 HTTP 경계 규칙으로 읽어 성공한 POST의 글을 메모리에 저장한 뒤 `201`과 JSON을 보낸다. `read_upstream_response()`가 상태 줄·헤더·선언된 본문 전체를 검증한다. `proxy_request()`가 백엔드 FD를 닫고 `send_response()`가 확인한 상태·본문을 클라이언트 FD에 보낸다. 송신은 전송이 끝날 때까지 남은 바이트를 반복 처리한다.
6. 작업자가 클라이언트 FD를 닫고 요청 메모리의 수명이 끝난다. 브라우저는 등록 성공 후 `GET /api/messages`로 목록을 다시 읽는다. 프록시가 502/504를 반환하면 화면에 요청 오류를 표시한다. 정적 GET은 같은 서버에서 계속 처리된다.

팀원이 설명할 연결점은 `accept()`의 블로킹과 논블로킹 동작, FD가 가리키는 두 독립 TCP 연결, 헤더와 본문 경계, DB 없이 유지하는 상태다. 스레드 풀은 스레드 수와 요청 메모리를 제한하며 느린 소켓 한 개가 작업자 한 자리를 차지한다. 프로세스 방식은 격리에 이점이 있고 이벤트 루프는 많은 유휴 연결을 다루기 쉽다. 정방향 프록시는 클라이언트를, 리버스 프록시는 서비스의 백엔드를 대신한다. 다중 백엔드·캐싱·접근 제어는 팀 확장 토론 주제다.

## 2턴 모듈 경계

| 담당 | 파일 | 호출 규약 |
| --- | --- | --- |
| A · TCP와 작업 큐 | `src/server.c`, `src/tcp.c`, `src/tcp.h` | `serve_tcp(name, bind_address, port, handler, context)`는 주소와 연결 처리 함수를 받아 정상 종료 시 0, 초기화·수락 실패 시 -1을 반환한다. `handler(fd, read_deadline, context)`는 FD를 빌린다. `receive_bytes()`는 수신 바이트 수 또는 EOF 0, `send_all()`은 완전 송신 시 0을 반환한다. 둘 다 I/O 오류에서 -1을 반환하고 시간 초과는 `errno=ETIMEDOUT`이다. 기한은 단조 시계의 절대 밀리초다. |
| B · HTTP와 정적 파일 | `src/http.c`, `src/http.h`, `src/static.c`, `src/static.h`, `public/` | `read_request(fd, &request, deadline)`은 완성된 요청에 200, 소켓 단절 등에 0, 그 외에는 보낼 오류 상태를 반환한다. `read_upstream_response(fd, &response, deadline)`는 `UPSTREAM_OK`, `UPSTREAM_INVALID`, `UPSTREAM_TIMEOUT` 중 하나다. `send_response()`, `send_error()`, `serve_static()`은 응답 송신을 시도해 0 또는 -1을 반환한다. |
| C · 라우팅과 백엔드 | `src/proxy.c`, `src/proxy.h`, `src/backend.c` | `route_request(client_fd, &request, &config)`는 파싱된 요청과 읽기 전용 `server_config`를 받아 정적 파일 또는 프록시 응답을 보내고 0 또는 -1을 반환한다. 백엔드는 `handle_backend_connection()`으로 A·B의 TCP와 HTTP 코드를 재사용한다. `handle_messages()`는 GET에 `send_message_list()`, POST에 `register_message()`를 호출한다. 글 저장과 목록 직렬화는 같은 뮤텍스로 보호하고 소켓 송신은 잠금 밖에서 한다. |

`struct request`의 `method`, `target`, `content_type`, `body.data`는 자신이 가진 `bytes` 배열을 가리킨다. 작업자의 `handle_client()` 또는 백엔드 연결 함수가 반환하면 무효다. `struct upstream_response`도 `proxy_request()` 안에서만 사용한다. 두 본문은 `struct http_body`의 `data`와 `length`로 전달하며 NUL 종료를 가정하지 않는다. `server_config`와 정적 루트 FD는 앞단 `main()`이 작업자 join까지 유지한다.

`serve_tcp()`의 IPv4 주소는 네트워크 바이트 순서, 포트는 호스트 바이트 순서다. `deadline_after(ms)`는 현재 단조 시각에 밀리초를 더한 기한을 반환하며 시계 오류 시 -1이다. `wait_for_io(fd, events, deadline)`과 `set_nonblocking(fd)`는 성공하면 0, 실패하면 -1을 반환한다. `request_path_length()`는 `?` 앞의 경로 바이트 수를 반환한다. `send_response_head()`는 상태·MIME·본문 길이와 선택적인 `Allow` 값을 받아 헤더만 보내고, 정적 응답은 그 뒤 파일을 직접 읽어 보낸다.

`accept_clients()`가 연 브라우저 FD는 큐를 통해 `worker_main()`으로 전달되고 작업자가 처리 직후 닫는다. 큐 포화나 종료 시 대기 중인 FD는 수락 스레드가 닫는다. `proxy_request()`가 연 백엔드 FD는 같은 함수가 성공·실패 모두에서 닫는다. 백엔드 프로세스는 자기 쪽에서 수락한 FD를 자기 작업자가 닫는다. 앞단 로그의 `front client_fd=… accepted`, `proxy client_fd=… backend_fd=… opened/closing`, `front client_fd=… closing`으로 한 요청의 두 FD를 연결할 수 있다.

## 기본값과 실패 처리

| 항목 | 기준값과 이유 |
| --- | --- |
| 포트·설정 | 앞단 `0.0.0.0:8080` (`--port`), 호스트 테스트 공개는 `127.0.0.1:8080`. 정적 루트 `/app/public` (`--static-root`), API 접두사 `/api/` (`--route-prefix`), 백엔드 `127.0.0.1:8001` (`--backend-address`, `--backend-port`). Compose에서 백엔드는 앞단의 네트워크 네임스페이스를 공유하고 8001은 호스트에 공개하지 않는다. 기본 주소가 고정돼 백엔드 중지 중에도 앞단을 띄우고, 요청 경로에서 DNS 해석에 대기하지 않는다. |
| 연결 종료 | 클라이언트·백엔드 TCP 연결마다 요청 하나, `Connection: close`. 응답 본문 뒤 연결을 닫는다. 뒤따르는 파이프라인 요청은 처리하지 않는다. 클라이언트와 백엔드의 연결을 별도로 회수하므로 중계 실패가 정적 요청에 영향을 주지 않는다. |
| 큐 | 작업자 4개, FIFO 최대 16개, `listen` backlog 64. 소규모 데모에 필요한 동시성과 메모리 상한을 제공한다. 큐 포화 시 수락 스레드가 50ms 한도 내에서 503 송신을 시도하고 연결을 닫는다. |
| 요청 상한 | 헤더 전체 8 KiB, 요청 대상 2 KiB, 본문 4 KiB. 장문 헤더는 431, 긴 대상은 414, 큰 본문은 413. 요청 줄이 헤더 한도를 넘거나 헤더 한도를 넘긴 요청의 대상이 2 KiB를 넘어도 414다(RFC 9112 §3). 요청 대상은 origin-form과 `http://` absolute-form을 받으며, absolute-form은 경로·질의만 남겨 라우팅하고 Host 값 대신 대상의 authority를 쓴다(RFC 9112 §3.2.2). Host 헤더는 형식과 관계없이 필수다. HEAD는 GET과 같은 상태·헤더를 본문 없이 보내고 백엔드에는 GET으로 묻는다(RFC 9110 §9.1, §9.3.2). 중복·잘못된 `Content-Length`, `Content-Length`와 `Transfer-Encoding` 혼용, 헤더·본문 도중 EOF는 400. 전송 인코딩 단독 요청은 지원 범위 밖이므로 501, `Expect`는 417. POST는 길이 선언이 필요하며 기준 엔드포인트는 UTF-8 평문 한 줄을 받는다. |
| 앞단 시간 | 읽기는 수락 시각부터 2초, 쓰기는 응답 시작부터 2초. `CLOCK_MONOTONIC` 절대 기한을 사용해 바이트를 조금씩 보내도 대기 시간이 리셋되지 않는다. 읽기 시간 초과는 408, 쓰기 실패는 연결을 닫는다. |
| 백엔드 시간·크기 | 연결 500ms, 요청 송신 1초, 응답 전체 수신 2초. 로컬 데모에서 느린 요청이 작업자를 오래 점유하지 않게 한다. 응답 헤더는 8 KiB, 본문은 64 KiB로 제한한다. 상태 줄과 단일 `Content-Length`를 확인하고 정확한 선언 길이를 모두 받은 뒤 브라우저로 보내므로, 불완전한 응답을 200으로 전달하지 않는다. 연결 거부·조기 EOF·잘못된 프레이밍·한도 초과는 502, 백엔드 시간 초과는 504. `Transfer-Encoding`, 길이 없는 응답, 1xx 및 204/304처럼 전용 처리가 필요한 응답은 기준 API에서 502로 처리한다. |
| 정적 파일 | 디코딩한 경로를 루트 FD 기준으로 한 구성요소씩 `openat()`으로 열고 `O_NOFOLLOW`를 사용한다. 점 경로·심볼릭 링크·일반 파일이 아닌 대상은 403, 없음은 404, 예상 밖 I/O 오류는 500. MIME은 확장자로 정한다. |
| 저장·화면 | 글당 UTF-8 최대 240바이트, 최대 100개. 가득 차면 409를 반환해 기존 글을 보존한다. GET은 200과 JSON 목록, POST는 201과 JSON 한 건을 반환한다. 브라우저는 글을 텍스트로 렌더링하고 GET으로 서버 상태를 다시 읽는다. 새로고침 후에도 글이 보이고, 백엔드 재시작 시 사용자 등록 글이 사라져 초기 예시 글로 돌아간다. |
| 종료 | SIGPIPE를 무시하고 송신 오류를 처리한다. SIGINT/SIGTERM 시 리스너와 대기 큐의 FD를 닫고 작업자를 깨운 다음, 활성 작업을 기존 기한 안에 종료하고 join한다. 프록시는 성공·실패 모두에서 자신이 연 백엔드 FD를 닫는다. 정적 파일과 루트 FD도 소유자가 닫는다. |

과제의 RFC 7230–7235 참고 범위에서 메시지 경계는 [RFC 9112](https://www.rfc-editor.org/rfc/rfc9112.html), 메서드와 상태 코드 의미는 [RFC 9110](https://www.rfc-editor.org/rfc/rfc9110.html)을 따른다. 기준 버전은 브라우저가 사용하는 origin-form과 길이가 명시된 메시지를 중심으로 구현한다.
