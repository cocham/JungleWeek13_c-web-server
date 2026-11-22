# 1. 기준 커밋 만들고 공유하기

[목차](README.md) · [다음 문서 — 브랜치와 PR](02-pull-requests.md)

## 공통 컴퓨터에서 준비

1. 세 명이 GitHub 저장소, Issues, PR에 접근할 수 있는지 확인한다. 기준 PR 작성자 한 명과 리뷰어 두 명을 정하고, 각 담당 이슈에 담당자를 지정한다. [#23 협업 규칙](https://github.com/cocham/JungleWeek13_c-web-server/issues/23)
2. 공통 헤더, 함수 시그니처, 버퍼와 소켓 해제 책임을 합의한다. 빌드 환경, 서버 및 백엔드 실행 방법도 정한다. [#1 공통 설계](https://github.com/cocham/JungleWeek13_c-web-server/issues/1) · [#22 승인 기준](https://github.com/cocham/JungleWeek13_c-web-server/issues/22)
3. 기준 구현을 리뷰할 브랜치에 올리고 아래 검증을 실행한다. 정확한 빌드·실행·재현 명령과 알려진 한계를 프로젝트 README에 기록한다. 필수 항목이 실패하면 재현 명령과 담당자를 남기고 기준 태그 발행을 보류한다. [#2 기준 구현](https://github.com/cocham/JungleWeek13_c-web-server/issues/2) · [#3 실행 검증](https://github.com/cocham/JungleWeek13_c-web-server/issues/3)

| 확인 항목 | 기대 결과 |
| --- | --- |
| `make`, `make test` | 빌드와 준비한 테스트가 통과한다. |
| `GET /index.html` | 정적 HTML이 예상한 MIME 타입과 응답 길이로 돌아온다. |
| `POST /api/messages` | UTF-8 평문 본문이 백엔드까지 전달되고 등록 응답 201이 돌아온다. |
| `GET /api/messages` | 백엔드 목록에 새 글이 포함되고 C 서버를 거쳐 클라이언트에 도착한다. |
| 동시 요청 2개 | 두 요청이 완료되고 로그나 관찰 결과로 worker 사용을 확인할 수 있다. |

## 병합하고 태그 발행

기준 PR을 리뷰한 뒤 squash merge한다. GitHub PR 화면에서 `main`에 들어간 병합 커밋 SHA를 확인한다. 아래 `MERGED_SHA`를 그 값으로 바꾸고, 해당 커밋을 체크아웃해 스모크 테스트를 실행한 뒤 태그를 발행한다.

```bash
git fetch origin
git switch --detach MERGED_SHA
# 프로젝트 README의 스모크 테스트를 실행한다.
git tag -a baseline-v0 MERGED_SHA -m "Verified shared baseline"
git push origin baseline-v0
```

## 팀원 세 명의 실행 확인

```bash
git fetch origin --tags
git switch --detach baseline-v0
git rev-parse HEAD
```

세 명이 SHA를 비교하고 README에 적힌 동일한 명령으로 빌드와 서버 실행을 확인한다. #23에 기준 태그와 SHA, 각 팀원의 실행 결과, 남은 제한 사항을 기록한다. 확인을 마친 뒤 담당별 첫 브랜치를 만든다. [#23 완료 조건](https://github.com/cocham/JungleWeek13_c-web-server/issues/23)
