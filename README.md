# 한 줄 방명록

C와 POSIX 소켓 API로 만든 HTTP/1.1 실습 서버다. 앞단 `front`는 HTML·CSS·JavaScript를 직접 제공하고 `/api/` 요청을 C 백엔드 `backend`로 중계한다. 두 프로그램은 같은 소스를 Docker Linux에서 빌드해 각각 실행한다.

## 빌드·실행·종료

Docker Desktop의 Linux 컨테이너 엔진 또는 Linux의 Docker Engine과 Docker Compose가 필요하다. 프로젝트 루트에서 실행한다.

```sh
docker compose build front
docker compose up -d --no-build
docker compose ps
```

브라우저에서 [http://127.0.0.1:8080/](http://127.0.0.1:8080/)을 연다. Dockerfile은 `make`로 `build/guestbook`과 `build/guestbook-backend`를 만든다. 두 컨테이너는 같은 네트워크 네임스페이스를 사용하며, 앞단은 8080 포트를 공개하고 백엔드는 그 안에서 `127.0.0.1:8001`에 바인딩한다. 호스트에서 8080을 사용 중이면 실행할 셸에서 환경 변수 `GUESTBOOK_PORT`를 다른 번호로 지정한다. [Docker Compose 네트워크 모드](https://docs.docker.com/reference/compose-file/services/#network_mode)

```sh
docker compose logs -f front backend
docker compose down
```

`logs -f`는 `Ctrl+C`로 로그 보기만 종료한다. `down`은 두 컨테이너와 프로젝트 네트워크를 정리한다. 방명록은 백엔드 프로세스 메모리에 저장된다. 브라우저를 새로고침하면 저장한 글이 다시 보이고, 백엔드를 재시작하거나 Compose를 내렸다 올리면 예시 글 한 건으로 초기화된다. 예시를 포함해 최대 100개를 보관하며, 글 하나의 한도는 UTF-8 240바이트다.

## API

| 요청 | 입력 | 성공 응답 |
| --- | --- | --- |
| `GET /api/messages` | 본문 없음 | `200`과 `{"messages":["…"]}` |
| `POST /api/messages` | UTF-8 평문 한 줄, `Content-Type: text/plain; charset=utf-8`, 바이트 단위 `Content-Length` | `201`과 `{"message":"…"}` |

빈 글, 잘못된 UTF-8, 개행이 포함된 글은 400이다. 본문 길이 선언이 없는 POST는 411, 길이 초과는 413, 지원 범위 밖의 미디어 유형은 415, 저장 공간이 가득 차면 409다. 백엔드 오류가 나면 화면에서 조회·등록 오류를 확인할 수 있다. 등록 시간이 초과되면 입력값을 유지하므로, 다시 등록하기 전에 목록에서 결과를 확인할 수 있다.

```sh
curl -i -H "Content-Type: text/plain; charset=utf-8" --data-binary "hello" http://127.0.0.1:8080/api/messages
curl -i http://127.0.0.1:8080/api/messages
```

Windows PowerShell에서는 실행 파일 이름을 `curl.exe`로 적는다.

## 글 등록 한 번을 따라가기

1. `public/app.js`의 `submitMessage()`가 `POST /api/messages`를 보낸다. `src/tcp.c`의 `accept_clients()`가 브라우저 TCP 연결을 `accept()`하고 클라이언트 FD를 FIFO에 넣는다. `worker_main()`이 FD를 빌려 `src/server.c`의 `handle_client()`를 호출한다.
2. `src/http.c`의 `read_request()`가 `read_head()`로 헤더 경계를 찾고 `read_body()`로 선언된 길이만큼 읽는다. 이때 `src/tcp.c`의 `receive_bytes()`가 클라이언트 FD에 `recv()`를 호출한다. 헤더와 같이 먼저 도착한 본문은 그대로 유지한다.
3. `src/proxy.c`의 `route_request()`가 `/api/`를 골라 `proxy_request()`를 호출한다. `proxy_request()`가 백엔드 FD를 `socket()`으로 만들고 `connect_backend()`로 8001에 연결한다. `send_upstream_request()`가 헤더를 다시 만들고 `src/tcp.c`의 `send_all()`을 사용해 원래 본문을 보낸다.
4. 백엔드도 `accept_clients()` 및 `worker_main()`을 사용한다. 이 연결의 백엔드 프로세스 쪽 FD는 `src/backend.c`의 `handle_backend_connection()`으로 넘어간다. `handle_messages()`가 `register_message()`로 보내고, `store_message()`가 뮤텍스 안에서 글을 저장한다. `send_json()`과 `src/http.c`의 `send_response()`가 201을 전송한다. 백엔드 작업자는 자기 프로세스의 FD를 닫는다.
5. 앞단의 `read_upstream_response()`가 상태 줄·헤더·본문을 확인한다. `proxy_request()`가 자기 백엔드 FD를 닫고 `send_response()`로 클라이언트 FD에 201과 JSON을 보낸다. `worker_main()`이 마지막에 클라이언트 FD를 닫는다. 브라우저의 `submitMessage()`는 성공 응답을 받은 뒤 `loadMessages()`로 최신 목록을 요청한다.

앞단 로그에서 `front client_fd=… accepted`, 같은 번호를 포함한 `proxy client_fd=… backend_fd=… opened/closing`, `front client_fd=… closing`을 순서대로 찾으면 된다. FD 번호는 프로세스별이며 연결을 닫으면 나중에 재사용된다.

## 경계와 오류 처리

- TCP에서 한 번 읽은 양은 요청 경계와 무관하다. `\r\n\r\n`과 `Content-Length`로 경계를 확인하고 송신은 남은 바이트를 반복해 처리한다. 연결당 요청 하나와 `Connection: close`를 사용한다. 요청 줄 8000 옥텟, 요청 헤더 필드 8 KiB, 요청 본문 4 KiB에 상한을 둔다.
- 앞단 읽기·쓰기는 각각 2초다. 백엔드 연결은 0.5초, 송신은 1초, 응답 전체 수신은 2초까지 기다린다. 큐 16칸이 가득 차면 503 송신을 시도하고 연결을 닫는다.
- 연결 거부, 응답 상태·헤더 오류, 8 KiB 헤더 또는 64 KiB 본문 한도 초과, 선언 길이 전의 EOF는 502다. 백엔드 연결·송신·수신 기한이 지나면 504다. 프록시는 단일 `Content-Length`를 가진 HTTP/1.1 백엔드 최종 응답을 처리하며 chunked·종료로 구분하는 응답·1xx·204·304는 502로 처리한다.
- 정적 파일은 루트 FD에서 시작해 `openat()`으로 각 경로 요소를 열고 심볼릭 링크를 거부한다. `SIGINT`·`SIGTERM`은 수락을 멈추고 대기 큐를 닫은 뒤 작업자 종료를 기다린다. 버퍼 수명과 예외 경로의 FD 정리 규칙은 `DESIGN.md`에 있다.

## Docker에서 자동 검사

위 명령으로 앞단과 백엔드를 시작한 뒤 실행한다. 검사 컨테이너는 Python 표준 라이브러리만 사용한다.

```sh
docker compose run --rm --no-deps smoke
```

HTML·CSS·JavaScript MIME, 없는 파일 404, 분할 POST와 재조회, 입력 오류, 동시 GET을 확인한다. 정상 검사 한 번마다 검증용 글 한 건이 방명록에 남는다.

실패 경로 검사는 테스트용 방명록에서 실행한다. 백엔드 중지로 현재 저장된 글이 초기화된다.

```sh
docker compose stop backend
docker compose run --rm --no-deps smoke upstream-failure
docker compose start backend
```

연결이 거부되면 502, 응답이 오지 않으면 504, 본문이 잘린 응답은 502인지 확인한다. 각 실패 뒤 앞단 정적 페이지도 다시 요청한다. 팀의 과제 확장은 `DESIGN.md`의 과제 범위표, 단계별 검사 결과는 `PROGRESS.md`에서 확인한다.

아래 명령은 임시 컨테이너에서 앞단과 백엔드를 따로 띄워 종료를 검증한다. 정적·API 요청을 반복한 뒤 `/proc/<PID>/fd`의 개수를 비교한다. 이어서 프로세스마다 요청 8개를 열어 둔 채 종료 신호를 보내고, 연결 종료와 리스너 해제, 종료 코드 0을 확인한다.

```sh
docker compose run --rm --no-deps -v ./tests:/checks:ro --entrypoint timeout front -k 2 20 bash /checks/shutdown.sh
```

Linux 빌드 환경에서는 `make test`도 두 바이너리를 빌드한 뒤 같은 종료 검사를 실행한다. 이 대상은 루프백 포트 8080과 8001을 사용한다.

## 팀별 후속 작업

| 담당 | 바꿀 파일 | 개선할 동작 | 완료 확인 방법 |
| --- | --- | --- | --- |
| A · TCP와 동시성 | `src/tcp.c`, `src/tcp.h`, `tests/shutdown.sh` | 큐 포화 횟수와 대기 시간을 기록해 작업자·대기열 크기의 영향을 측정한다. | `wrk`로 동시 연결 1·4·16·64의 처리량, 95백분위 지연, 503 수를 기록한다. 같은 설정에서 종료 검사와 FD 기준값 복귀를 다시 확인한다. |
| B · HTTP와 정적 파일 | `src/http.c`, `src/static.c`, `Makefile`, 새 `tests/test_http.c` | 파서와 파일 응답의 경계값을 독립적으로 검증한다. | `socketpair()`로 분할 헤더·중간 EOF·잘못된 길이를 검사한다. 정확한 한도와 한도 초과, 파일 종류별 MIME, 루트 안팎의 심볼릭 링크를 단위 테스트로 고정한다. ASan·LSan 빌드에서도 같은 모음을 통과시킨다. |
| C · 라우팅과 프록시 | `src/proxy.h`, `src/proxy.c`, `src/server.c`, `compose.yaml`, `tests/smoke.py` | 경로별로 두 백엔드를 설정하되 방명록의 GET과 POST는 같은 메모리 저장소로 보낸다. | 두 경로가 각각 지정한 백엔드에 도달하는지 로그로 확인한다. 한 백엔드를 중지한 뒤 해당 경로의 502, 다른 경로와 정적 페이지의 200을 검증한다. |
