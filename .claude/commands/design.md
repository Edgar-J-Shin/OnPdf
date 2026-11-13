# Role: System Architect

$ARGUMENTS 에 대한 기술 설계 + 구현 계획을 수립한다.

## 프로세스

1. `git branch --show-current`로 현재 브랜치명(`{branch}`)을 확인한다
2. `.claude/workflow/{branch}/01-plan.md` PRD를 Read한다 (없으면 `/planning` 먼저 실행 안내)

### Stage A: 코드베이스 분석 (Explore 에이전트, 최대 3개 병렬)

작업 범위가 넓을 때 레이어별로 에이전트를 분리한다:
- Agent 1: core/ (Document, Page, 렌더 캐시, 텍스트 인덱스, engine 래퍼)
- Agent 2: edit/ (Command, 실행 취소 스택, 선택 모델, 폰트 매니저)
- Agent 3: app/ · cli/ (Qt UI, 워커 스레드 연동, 명령줄 도구)

각 에이전트 수행 사항:
1. 영향 파일 탐색: 변경 대상 + 직접 의존 파일 식별
2. 레이어 의존성 검증: `#include` 검사 → Layer Dependency Rules 위반 여부 (core 의 Qt include, engine 밖의 PDFium include)
3. 재사용 자산 탐색: 기존 RAII 래퍼, Command 베이스, 유틸, 테스트 픽스처
4. 제약 조건 확인: 필요한 `FPDF*` API 와 PDFium 고정 버전 지원 여부, 스레드 제약, Windows 외 플랫폼 빌드 영향

### Stage B: 아키텍처 설계 (Plan 에이전트)

Explore 결과를 입력으로:
1. 아키텍처 결정: 변경 레이어, 새 클래스/인터페이스, Command 로 표현할 변경 단위
2. 스레드 설계: 어느 호출이 PDFium 워커로 가는지, UI 가 받는 결과 타입, 취소/우선순위
3. 데이터 흐름: 파일 → PdfiumEngine → Document/Page → (Command) → 렌더 캐시 → UI
4. 에러 처리 전략: PDFium 실패 반환, 손상 파일, 저장 검증 실패 시 복구
5. 테스트 설계: 단위(실행→취소→재실행) / 렌더링 회귀(기준 PNG 추가) / 왕복 중 무엇을 추가할지
6. **파일별 구현 계획**: core → edit → cli/tests → app 순서로 상세 명세

## 산출물 구조 (`02-design.md`)

```markdown
# [기능명] 기술 설계

## 영향 분석
- 영향 파일: [경로] + [변경 사유]
- 재사용 자산: [파일/클래스명] + [활용 방안]
- 레이어 의존성: 위반 여부
- PDFium API: 사용할 함수 목록 + 고정 버전 지원 여부
- 제약/리스크: [항목] + [상세]

## 아키텍처 결정
- 신규 클래스/인터페이스, Command 목록

## 스레드·데이터 흐름
- [UI 요청] → [워커 큐] → [PdfiumEngine] → [결과] → [캐시/UI]
- 소유권 (unique_ptr/shared_ptr/참조) 명시

## 에러 처리
- [시나리오]: [처리 방법]

## 테스트 계획
- 단위 / 렌더링 회귀 / 왕복: 추가할 테스트와 testdata

## 구현 계획 (파일별)
구현 순서: core/ → edit/ → cli/ · tests/ → app/
각 파일별: [파일 경로]: [변경 내용 상세]
```

## 준수사항
- CLAUDE.md 핵심 설계 원칙 6개 (core 무UI, 단일 스레드, Command, RAII, 원본 보존, JS 비활성) 최우선
- Layer Dependency Rules 준수
- 기존 래퍼/유틸 재사용 극대화
- 구현 계획은 `/develop`에서 바로 코딩할 수 있을 정도로 상세하게

## 완료 후 승인 절차 (필수 실행)

아래 절차를 **반드시** 순서대로 실행한다. 생략 금지.

### Step 1: 완료 보고
산출물 경로와 핵심 요약을 텍스트로 출력한다.

### Step 2: 다음 단계 확인 (AskUserQuestion 필수 사용)
**반드시 `AskUserQuestion` 도구를 호출**하여 사용자에게 묻는다:

- question: "다음 단계를 선택해주세요."
- options: ["다음 단계 진행 (/develop)", "수정 후 재실행", "여기서 중단"]

### Step 3: 응답에 따른 분기
- **"다음 단계 진행"** → `Skill` 도구로 `/develop` 명령을 **즉시** 실행한다
- **"수정 후 재실행"** → 사용자 피드백을 반영하여 현재 단계 재수행
- **"여기서 중단"** → 워크플로우 중단 안내
