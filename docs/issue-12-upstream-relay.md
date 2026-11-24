# #12 리버스 프록시 요청 중계와 검증

#11에서 확인한 두 TCP 연결을 기준으로, 프런트가 백엔드 주소를 구하는 과정부터 요청을 재구성하고 응답을 돌려주는 과정까지 정리함. 1단계에서 호스트명 지원을 추가했고, 2~4단계에서는 FD 정리, 헤더 전달, 업스트림 오류와 분할 응답을 살펴봄. [이슈 #12](https://github.com/cocham/JungleWeek13_c-web-server/issues/12)

## 1단계 — 백엔드 호스트명을 연결 주소로 바꿈

기존 `inet_pton()`은 `127.0.0.1` 같은 숫자 IPv4를 받았음. 서비스 이름으로 백엔드를 지정할 수 있도록 `server.c`에서 옵션을 모두 읽은 뒤 `resolve_backend()`가 `getaddrinfo()`를 호출하도록 변경함.

```text
프런트 시작    localhost + 기본 포트 8001 → getaddrinfo() → config.backend에 주소 복사
/api/ 요청     새 backend_fd 생성 → 저장한 주소로 connect() → 요청 중계
```

첫 IPv4 TCP 주소를 `config.backend`에 복사한 뒤 해석 결과 목록을 해제함. 현재 구현은 서버 시작 시 한 번 해석하므로 백엔드 IP가 바뀌면 프런트를 재시작해야 함.

| `--backend-address` | 직접 확인한 결과 |
|---|---|
| `localhost` | `GET /api/messages` → `200 OK`와 방명록 JSON |
| `127.0.0.1` | 같은 요청에 `200 OK`와 같은 JSON |
| `no-such-backend.invalid` | 주소 해석 오류를 출력하고 시작 단계에서 종료 코드 `1` |

## 2단계 — 두 소켓을 누가 닫는지 확인

```text
클라이언트 ── TCP ① ── 프런트 ── TCP ② ── 백엔드
                        │  │
               client_fd  backend_fd
```

- 프런트의 `client_fd`는 브라우저 연결을 `accept()`해서 얻음. 워커가 요청 처리를 마친 뒤 `close_client()`로 닫음.
- 프런트의 `backend_fd`는 백엔드에 접속하려고 `socket()`으로 만듦. 같은 프런트 워커가 `proxy_request()` 안에서 닫음. 연결이 거부되면 `backend_fd`를 닫고 `502`를 보낸 뒤 `client_fd`를 닫음.
- 연결에 성공하면 백엔드도 자기 쪽에서 `accept()`한 FD를 얻음. 백엔드 워커가 요청 처리를 마치면 그 FD를 닫음. 연결 거부 시에는 백엔드가 받아들인 FD가 없음.

| 재현한 경로 | 결과 |
|---|---|
| GET 64건 | 요청 전후 열린 FD 수가 프런트 `5 → 5`, 백엔드 `4 → 4` |
| 백엔드 연결 거부 32건 | 전부 `502`. 프런트 FD 수 `5 → 5`, 업스트림 소켓 열기와 닫기 각각 32회 |

프런트의 기본 FD 5개는 표준 입출력·에러 3개, 정적 파일 디렉터리 1개, 리스닝 소켓 1개임.

## 3단계 — 백엔드로 보내는 HTTP 요청 확인

```text
브라우저 요청라인 → request->target  → GET /api/messages?limit=5 HTTP/1.1
저장한 백엔드 주소 → IP와 포트 변환    → Host: 127.0.0.1:8001
읽어 둔 본문      → 실제 바이트 길이  → Content-Length: 5 + 본문 hello
```

- **요청라인**에서 경로와 쿼리가 함께 있는 `/api/messages?limit=5`는 origin-form임. `send_upstream_request()`가 `request->target`을 사용해 쿼리까지 넘김.
- **`Host`**는 해석된 백엔드 IP와 포트로 재구성함. `--backend-address localhost`를 줘도 이 환경의 업스트림 헤더는 `Host: 127.0.0.1:8001`임.
- **`Content-Length`**는 `request->body.length`로 만들고, 본문 전송에도 같은 길이를 사용함. 두 비교 요청의 업스트림 로그에서 `Content-Length: 5`와 `hello`를 확인함.

**`Connection` 테스트를 한 이유**는 첫 번째 TCP 연결에서만 쓰겠다고 표시된 헤더가 두 번째 TCP 연결에 넘어가는지 확인하기 위해서임. 두 요청 모두 `Content-Type: text/plain`과 본문 `hello`를 보냈고, 한 요청에만 `Connection: Content-Type`을 추가함.

| 추가한 헤더 | 업스트림 요청 로그 | 응답 |
|---|---|---|
| `Connection: Content-Type` | `Content-Type` 제거, 본문 `hello` 유지 | `415 Unsupported Media Type` |
| 추가 없음 | `Content-Type: text/plain`과 본문 `hello` 유지 | `201 Created`, `{"message":"hello"}` |

현재 코드는 `http.c`에서 `Connection`이 `Content-Type`을 지정하면 그 값을 전달 대상에서 제외함. 응답 코드의 차이는 백엔드가 본문 형식 정보를 받았는지에 따라 생김.

## 4단계 — 업스트림 연결 실패, 지연, 분할 응답

`proxy_request()`는 **연결 → 요청 전송 → 응답 읽기** 순으로 진행함. 응답을 읽을 때는 2초 기한을 사용함. 백엔드가 응답하지 않을 때 무한히 기다리거나, 덜 받은 본문을 정상 응답으로 처리하지 않는지 확인하는 단계임.

| 재현 상황 | 확인할 결과 |
|---|---|
| 백엔드가 꺼져 있어 연결 거부 | `502 Bad Gateway` |
| 가짜 백엔드가 요청을 받고도 응답하지 않음 | `504 Gateway Timeout`. 대기 중과 이후 정적 페이지는 `200` |
| `Content-Length: 10`을 보낸 뒤 본문 `ok`만 보내고 닫음 | 응답이 중간에 끝난 것을 감지해 `502` |
| `Content-Length: 5`를 보낸 뒤 `he`와 `llo`를 0.5초 간격으로 보냄 | `200 OK`와 본문 `hello` |

앞의 세 경우는 `tests/smoke.py upstream-failure`로 재현할 수 있음. 마지막 실습에서 공유한 출력에는 `200 OK`와 `Content-Length: 5`까지 있었음. 분할 수신 결과를 기록할 때는 curl에 **본문 `hello`까지 출력됐는지** 확인하면 됨.

## 남은 확인

- 분할 응답의 본문까지 기록하고, 업스트림 전송에서 부분 `send()`가 발생했을 때도 남은 바이트를 보내는지 별도로 확인함.
- B와 실제 백엔드를 띄워 POST 본문 전달과 응답 수신을 함께 재현함.
