# 2. 브랜치와 PR 운영하기

[목차](README.md) · [이전 문서 — 기준 커밋](01-baseline.md) · [다음 문서 — 통합 일정](03-integration-schedule.md)

## 첫 브랜치

팀원별 실행 확인을 마친 뒤 각자 자신의 브랜치만 만든다. 세 브랜치의 시작점은 `baseline-v0`다. [#23 브랜치 규칙](https://github.com/cocham/JungleWeek13_c-web-server/issues/23)

| 담당 | 명령 | 담당 이슈 |
| --- | --- | --- |
| A · TCP와 동시성 | `git switch -c feat/tcp-concurrency baseline-v0` | [#19](https://github.com/cocham/JungleWeek13_c-web-server/issues/19) |
| B · HTTP와 정적 파일 | `git switch -c feat/http-static baseline-v0` | [#20](https://github.com/cocham/JungleWeek13_c-web-server/issues/20) |
| C · 리버스 프록시 | `git switch -c feat/reverse-proxy baseline-v0` | [#21](https://github.com/cocham/JungleWeek13_c-web-server/issues/21) |

## PR 작성

이슈 하나를 PR 하나의 기본 단위로 잡는다. 공통 헤더나 함수 시그니처를 수정하기 전에는 영향을 받는 담당자와 변경 내용 및 병합 순서를 합의한다. [#23 PR 규칙](https://github.com/cocham/JungleWeek13_c-web-server/issues/23)

PR 제목과 본문은 [팀 작업 공통 양식](04-work-formats.md#4-pr)의 예시를 사용한다.

## 리뷰와 병합

1. `git status --short`로 빠진 파일이 없는지 확인한다. `git fetch origin` 후 `git log --oneline origin/main..HEAD`와 `git diff --stat origin/main...HEAD`로 PR의 커밋과 파일을 확인한다.
2. 최신 `origin/main`을 작업 브랜치에 통합하고 충돌이나 빌드 실패를 해결한다. 통합 상태에서 스모크 테스트를 실행하고 결과를 PR에 남긴다.
3. 교차 리뷰에서 결과를 확인한 뒤 squash merge한다. 다음 이슈를 시작할 때 `git fetch origin` 후 `git switch -c <새-브랜치> origin/main`으로 새 브랜치를 만든다. 명령의 `<새-브랜치>`는 실제 이름으로 바꾼다. [#23 병합 규칙](https://github.com/cocham/JungleWeek13_c-web-server/issues/23)
