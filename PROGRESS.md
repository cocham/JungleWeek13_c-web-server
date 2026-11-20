# 진행 기록

## 1턴 · 2026-11-20

- **변경** — `HARNESS.md`와 과제 원문을 확인했다. `work.md` 사본의 SHA-256은 원문과 일치했다. `DESIGN.md`에 과제 범위, 등록 요청의 FD 흐름, 설정과 한도를 정리하고 `src/server.c`와 Linux `Dockerfile`로 `GET /` 첫 응답을 구현했다. 현재 연결은 수락 순서대로 한 건씩 처리한다.
- **빌드·실행** — 작업 루트에서 `docker build --no-cache -t team2-guestbook:turn1 .`와 `docker run -d --publish 127.0.0.1::8080 team2-guestbook:turn1`을 실행했다. Docker `desktop-linux`에서 C17 빌드가 경고를 오류로 처리하는 설정으로 성공했다. `docker port <컨테이너 ID> 8080/tcp`가 알려 준 `127.0.0.1:61364`에 `curl.exe --noproxy '*' --http1.1 -i http://127.0.0.1:61364/`로 요청해 `200 OK`, `text/plain; charset=utf-8`, `Content-Length: 29`, `Connection: close`, 본문 `Guestbook server is running.`을 확인했다.
- **소켓·종료 검사** — `docker exec -i <컨테이너 ID> bash -s`로 컨테이너 안에서 분할한 헤더와 본문의 마지막 바이트가 도착한 뒤 200을 확인했다. 없는 경로 404, 음수 길이 400, 4 KiB 초과 길이 413, 미지원 전송 인코딩 501, 헤더 수신 지연 408도 확인했다. `docker stop --timeout 4` 후 `docker inspect` 결과는 `exited 0`이었고 테스트 컨테이너를 삭제했다.
- **남은 일** — 2턴에 A·B·C 소스와 인터페이스를 나누고 작업 큐, 정적 UI, 프록시를 통한 조회를 연결한다. 등록과 502/504는 3턴, 브라우저·동시 요청·백엔드 실패·종료 경로의 통합 검수는 4턴에 수행한다. 이번 턴에는 SIGTERM 정상 종료를 관찰했으며 FD·메모리 누수 검사는 이후 과제 검증에 포함한다.

## 2턴 · 2026-11-20

- **변경** — A의 TCP·4개 pthread 작업자·16칸 FIFO, B의 HTTP·정적 파일, C의 라우팅·리버스 프록시·C 백엔드를 분리했다. `DESIGN.md`에 공통 함수 계약, 버퍼 수명, 두 FD의 소유자를 기록했다. 정적 화면은 HTML·CSS·JS로 구성했고, 조회 실패 시 목록 오류를 표시한다.
- **빌드·조회** — 작업 루트에서 `docker build --no-cache -t team2-guestbook:turn2 .`로 앞단·백엔드를 빌드했다. `docker run -d --publish 127.0.0.1::8080 team2-guestbook:turn2`와 `docker run -d --network container:<앞단 ID> --entrypoint /usr/local/bin/guestbook-backend team2-guestbook:turn2`로 실행했다. curl에서 `/`, `/style.css`, `/app.js`가 각각 HTML·CSS·JavaScript MIME으로 200을 반환했고, `/api/messages?check=1`은 JSON으로 `방명록에 오신 것을 환영합니다!`를 반환했다. 앞단 로그에서 해당 요청의 `client_fd=5` 수락, `backend_fd=6` 열기·닫기, `client_fd=5` 닫기를 확인했다. CLI로 앞단 9080과 백엔드 9001을 설정한 추가 실행에서도 `GET /api/messages`가 200이었다.
- **큐·실패 검사** — `docker exec -i <앞단 ID> bash -s`로 확인한 PID 1의 스레드는 수락 스레드 포함 5개였다. 헤더 수신이 지연된 연결은 408, 동시에 보낸 정상 GET은 200이었다. 분할 헤더·본문은 200, 4개 작업자와 16칸 큐를 채운 뒤 추가 연결은 503이었다. 정적 점 경로 403, 없는 정적 파일과 백엔드 API 경로 404, 현재 POST 405를 확인했다. 백엔드 중지 후 조회는 502였고 `/`는 계속 200이었다. 두 프로세스가 종료 코드 0으로 끝났으며 테스트 컨테이너를 삭제했다.
- **남은 일** — 3턴에 POST 등록·메모리 목록, 504 구분, Makefile·Docker 실행 설정·테스트 스크립트·한국어 README를 추가한다. 실제 브라우저에서 UI를 조작하는 검수는 4턴에 진행한다.

## 3턴 · 2026-11-20

- **변경** — 등록 폼에서 UTF-8 평문을 보내고 앞단이 HTTP 경계를 확인한 뒤 백엔드에 전달한다. 백엔드는 뮤텍스로 최대 100개, 글당 240바이트의 메모리 목록을 보호한다. 화면에서 등록 결과와 조회 오류를 알려 준다. Makefile, `compose.yaml`, `tests/smoke.py`, 한국어 README를 추가했다.
- **빌드·등록 검사** — 작업 루트에서 격리된 Compose 프로젝트로 `docker compose build --no-cache front`, `docker compose up -d --no-build`, `docker compose run --rm --no-deps smoke`를 실행했다. 분할 POST가 원문 그대로 201을 반환하고 다음 GET에서 글을 확인했다. 정적 MIME과 404, 입력 오류 400·411·413·415, 동시 GET 16건의 200을 확인했다.
- **실패·용량 검사** — `docker compose stop backend` 후 `docker compose run --rm --no-deps smoke upstream-failure` 결과는 연결 거부 502, 응답 없음 504, 본문 조기 종료 502였다. 지연 중과 각 실패 직후 정적 페이지는 200이었다. `docker compose start backend` 뒤 예시 글만 남았고, 240바이트 글 99개를 더 저장해 총 100개를 조회했다. 추가 POST는 409, 목록 GET 본문은 47,877바이트였다. `docker compose down` 후 검사 프로젝트에 남은 컨테이너가 없었다.
- **남은 일** — 4턴에 README 실행 절차를 다시 확인하고 Playwright로 화면 열기·등록·새로고침을 조작한다. 종료 경로 자원 회수와 역할별 후속 작업도 기록한다.

## 4턴 · 2026-11-20

Docker Linux/amd64에서 `COMPOSE_PROJECT_NAME=team2-turn4-verify`로 검수했다. `docker compose build --no-cache front`, `docker compose up -d --no-build`, `docker compose ps`를 실행했고, 호스트의 `127.0.0.1:8080`에서 `/`가 200이었다.

| 검사 | 실행한 명령·조작 | 관찰값 |
| --- | --- | --- |
| 브라우저 | 설치된 Playwright 스킬의 `playwright_cli.sh`로 Linux Chromium 156에서 `open`, `snapshot`, `fill`, `click`, `reload`, `requests`, `console` 실행 | 등록 POST 201 → 목록 GET 200. 새로고침 뒤 `팀2 최종 검수 한 줄`이 한 번 표시됐다. 수정본은 별도 앞단 포트 8081에서 새 세션으로 다시 열었고 정상 흐름의 콘솔 오류·경고는 0건이었다. |
| 정적·API·동시성 | `docker compose run --rm --no-deps smoke` 및 임시 PNG·JPEG를 GET | HTML·CSS·JS의 MIME과 없는 파일 404 확인. PNG 122바이트, JPEG 661바이트는 MIME과 원본 SHA-256이 일치했다. 분할 UTF-8 POST의 원문이 다음 GET에 나타났고 동시 GET은 16/16건 200이었다. |
| 백엔드 장애 | `docker compose pause backend` 후 브라우저 등록, `unpause`·`stop backend` 후 `docker compose run --rm --no-deps smoke upstream-failure`, 마지막에 `docker compose start backend` | 지연 POST는 504, 중지 상태의 GET·POST는 502였다. 화면에 오류를 표시하고 등록 입력값을 유지했다. 본문 조기 종료도 502였고, 지연 중과 장애 직후 정적 페이지는 200이었다. |
| 종료·자원 | README의 `tests/shutdown.sh` 실행. 빌드 스테이지에서 `make clean` 후 `-O1 -g -fsanitize=address,leak -Werror`로 재컴파일해 같은 검사 반복 | 정적·프록시 GET 64건 전후 FD 수는 앞단 5→5, 백엔드 4→4였다. 프로세스당 미완성 연결 8개를 둔 종료에서 앞단 SIGTERM과 백엔드 SIGINT 모두 종료 코드 0, 모든 연결과 리스너 닫힘. 수락/닫기 로그는 각각 72/72, 40/40이었다. ASan·LSan 빌드도 통과했다. |

**수정과 재검증** — 브라우저가 기본 `/favicon.ico`를 요청해 발생한 404를 `public/index.html`의 빈 아이콘 지정으로 없앴다. ASan용 `-O1` 빌드에서 `src/static.c`의 `file_fd` 초기화 경고가 컴파일을 중단시켜 초기값을 지정했다. 두 수정 후 브라우저 등록·재조회와 README의 정상·장애·종료 검사를 다시 통과했다. `docker compose stop` 후 두 서비스 모두 `exited 0`, `PID=0`이었고 `docker compose down` 후 검수 프로젝트 컨테이너와 네트워크는 0개였다.

**인계** — README의 ‘팀별 후속 작업’ 표에 변경 파일과 완료 조건을 적었다. 기준 버전 이후의 과제로 정한 모듈별 전체 단위 검사, 다중 백엔드, 장시간 부하 측정과 macOS 이식성 검사는 이번 검수에서 실행하지 않았다. 개별 팀원 PC의 재현은 README 명령으로 인계한다.
