# 3. 날짜별 통합 일정

[목차](README.md) · [이전 문서 — 브랜치와 PR](02-pull-requests.md)

| 날짜 | 팀이 확인할 것 | 근거 |
| --- | --- | --- |
| 11/20 금 | 기준 구현을 검증한다. 세 명이 같은 SHA에서 같은 명령으로 서버를 실행하고 작업 브랜치를 만든다. | [#2](https://github.com/cocham/JungleWeek13_c-web-server/issues/2) · [#3](https://github.com/cocham/JungleWeek13_c-web-server/issues/3) · [#23](https://github.com/cocham/JungleWeek13_c-web-server/issues/23) |
| 11/21 토 | 소켓 읽기에서 HTTP 파싱과 백엔드 연결까지 한 요청을 따라간다. 정적 GET과 프록시 POST를 확인한다. | [#24](https://github.com/cocham/JungleWeek13_c-web-server/issues/24) |
| 11/23 월 | 스레드 풀과 큐, POST 본문과 오류 응답, 프록시 중계를 검토한다. 최신 `main`에서 동시 요청과 프록시 POST를 확인한다. | [#24](https://github.com/cocham/JungleWeek13_c-web-server/issues/24) |
| 11/24 화 | 백엔드 중단·타임아웃과 클라이언트 연결 종료를 실험한다. 최신 `main`에서 기본 오류 처리와 통합 결과를 확인한다. | [#24](https://github.com/cocham/JungleWeek13_c-web-server/issues/24) |
| 11/25 수 | 최종 `main`에서 단위·E2E·ASan·부하 테스트 결과를 모은다. 교차 리뷰를 마치고 세 명이 각자 전체 요청 흐름을 설명한다. | [#15](https://github.com/cocham/JungleWeek13_c-web-server/issues/15) · [#16](https://github.com/cocham/JungleWeek13_c-web-server/issues/16) · [#17](https://github.com/cocham/JungleWeek13_c-web-server/issues/17) · [#18](https://github.com/cocham/JungleWeek13_c-web-server/issues/18) |

## 45분 공동 세션

| 시간 | 진행 |
| --- | --- |
| 10분 | 한 명이 요청 한 건의 호출 경로와 FD 변화를 설명한다. |
| 15분 | 담당자가 그날 주제에 맞는 실패를 재현한다. |
| 10분 | 다른 두 명이 관련 소스 위치를 찾아 설명하고 질문한다. |
| 10분 | 최신 `main`을 스모크 테스트하고 PR 또는 당일 변경사항과 남은 문제를 확인한다. |

세션 후 #24에 검증한 커밋 SHA, 실행 명령, 관찰 결과, 남은 질문을 3~5줄로 남긴다. [#24 공동 세션 운영](https://github.com/cocham/JungleWeek13_c-web-server/issues/24)
