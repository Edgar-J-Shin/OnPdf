# Role: QA & Code Reviewer

$ARGUMENTS 에 대한 코드 리뷰를 수행한다.

## 프로세스

1. `git branch --show-current`로 현재 브랜치명(`{branch}`)을 확인한다
2. `.claude/workflow/{branch}/` 디렉토리의 모든 산출물을 Read한다
3. `git diff develop...HEAD`로 변경된 코드 전체를 확인한다
4. 체크리스트 기반 리뷰 수행

## 리뷰 체크리스트

### Architecture & Patterns
- [ ] Layer Dependency Rules 위반 여부 (`#include` 검사: core/edit 의 Qt, engine 밖 PDFium)
- [ ] 문서 변경이 Command(`execute`/`undo`) 로만 이뤄지는지
- [ ] 핸들 RAII 래퍼 사용, raw 핸들 노출 없음
- [ ] 플랫폼 분기 위치 (`core/platform/`, `app/` 만)

### Thread Safety & Memory
- [ ] PDFium 호출이 워커 스레드 밖에서 발생하지 않음
- [ ] 공유 상태 보호 (mutex / 메시지 전달), 데이터 레이스 가능성
- [ ] 소유권 명확 (unique_ptr/shared_ptr/참조), 댕글링·누수 가능성
- [ ] 큰 비트맵·캐시의 수명과 상한 (LRU 상한, 메모리 폭주 방지)

### Safety & Quality
- [ ] PDFium 반환값 검사 누락
- [ ] 손상·암호화·0쪽·거대 문서 처리
- [ ] 저장 경로: 임시 파일 → 검증 → 교체, 원본 보존, 증분 저장 조건
- [ ] UTF-8 ↔ UTF-16 변환 위치와 한글(CID) 처리
- [ ] 예외가 경계(UI·CLI) 밖으로 새지 않음

### Tests
- [ ] 새 Command 의 실행→취소→재실행 테스트 존재
- [ ] 렌더링 변경 시 기준 PNG 갱신·추가
- [ ] 왕복 테스트 영향 여부
- [ ] 추가된 testdata 의 라이선스 기록

### Consistency
- [ ] 기존 코드 패턴·네이밍 컨벤션(`PascalCase` 클래스, `snake_case` 함수, `name_` 멤버) 준수
- [ ] 사용자 노출 문자열 `tr()` + ko/en
- [ ] Windows 외 플랫폼에서 core/cli/tests 빌드 깨짐 없음

## 산출물 구조 (`04-review.md`)

```markdown
# [기능명] 코드 리뷰

## 요약
- 전체 평가 (Pass / Pass with comments / Changes required)
- 변경 파일 수 / 추가 라인 / 삭제 라인

## 이슈 목록
각 이슈: 심각도 (Critical/Warning/Info) + 파일:라인 + 설명 + 제안

## 잘한 점
- 코드 품질 측면에서 잘 된 부분

## 최종 판정
```

## 준수사항
- 심각도 분류: **Critical** (반드시 수정 — 스레드 위반, 누수, 원본 손상 가능성, 레이어 위반 포함) / **Warning** (권장 수정) / **Info** (참고)
- 개선 사항은 구체적 코드 제안 포함
- Critical 이슈가 없으면 Pass

## 완료 후 승인 절차 (필수 실행)

아래 절차를 **반드시** 순서대로 실행한다. 생략 금지.

### Step 1: 완료 보고
산출물 경로와 핵심 요약을 텍스트로 출력한다.

### Step 2: 최종 결과 확인 (AskUserQuestion 필수 사용)

#### Critical 이슈가 없는 경우
**반드시 `AskUserQuestion` 도구를 호출**하여 사용자에게 묻는다:

- question: "코드 리뷰 통과 (Critical 이슈 없음). 워크플로우를 완료할까요?"
- options: ["워크플로우 완료", "추가 수정 필요"]

#### Critical 이슈가 있는 경우
**반드시 `AskUserQuestion` 도구를 호출**하여 사용자에게 묻는다:

- question: "Critical 이슈가 발견되었습니다. 다음 단계를 선택해주세요."
- options: ["수정 후 재구현 (/develop)", "여기서 중단"]

### Step 3: 응답에 따른 분기
- **"수정 후 재구현"** → `Skill` 도구로 `/develop` 명령을 **즉시** 실행한다
- **"워크플로우 완료"** → 커밋·푸시 후 PR 이 없으면 `gh pr create --base develop` (본문에 `Closes #이슈번호`, 테스트 요약). PR 링크와 CI 상태를 안내하고 종료. 머지는 사용자가 결정
- **"여기서 중단"** / **"추가 수정 필요"** → 사용자 피드백 대기
