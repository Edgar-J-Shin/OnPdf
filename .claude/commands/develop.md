# Role: C++ Developer

$ARGUMENTS 에 대한 코드를 구현한다.

## 프로세스

1. `git branch --show-current`로 현재 브랜치명(`{branch}`)을 확인한다
2. `.claude/workflow/{branch}/02-design.md` 설계서를 Read한다 (없으면 `/design` 먼저 실행 안내)

### Stage 1: Validate (Explore 에이전트)

설계서 기반으로 구현 전 빠른 검증:
1. 설계서에 명시된 영향 파일들이 현재 코드베이스와 일치하는지 확인
2. 설계 이후 변경된 파일이 있는지 확인 (다른 브랜치 머지 등)
3. `#include` 의존성 사전 검증
4. 설계서의 `FPDF*` API 가 `third_party/` 고정 버전 헤더에 존재하는지 확인

**Output**: 설계서 유효성 확인 또는 불일치 사항 보고

### Stage 2: Implement (메인 에이전트 직접 수행)

설계서의 구현 계획에 따라 코딩한다.
구현 순서 엄수: **core → edit → cli/tests → app**

구현 시 체크리스트 (매 파일 작성 시 확인):
- [ ] PDFium 반환값(nullptr/FALSE/음수) 전부 검사
- [ ] 핸들은 RAII 래퍼로만 보유, raw `new`/`delete` 없음
- [ ] PDFium 호출이 워커 스레드 밖에서 일어나지 않음
- [ ] 문서 변경은 Command(`execute`/`undo`) 로만
- [ ] core/edit 에 Qt include 없음, engine 밖에 PDFium include 없음
- [ ] 문자열 UTF-8, PDFium UTF-16 변환은 engine 경계에서만
- [ ] 플랫폼 분기는 `core/platform/` 또는 `app/` 에만
- [ ] 저장 경로는 임시 파일 → 검증 → 교체
- [ ] 새 Command 마다 실행→취소→재실행 단위 테스트 추가
- [ ] 렌더링 변경 시 기준 PNG 추가/갱신
- [ ] 점진적 구현: 파일 단위 작성 → 컴파일 에러 없는 상태 유지

### Stage 3: Verify (메인 에이전트 직접 수행)

검증 파이프라인 순서:
1. **Build**: `cmake --build --preset dev` — 실패 시 에러 분석 → Stage 2 복귀. 경고는 기록하고 신규 경고는 수정
2. **Test**: `ctest --preset dev --output-on-failure` — 실패 시 수정 → Step 1부터 재실행
3. **Format**: `clang-format --dry-run --Werror` 로 변경 파일 검사 (`.clang-format` 있을 때)
4. **Layer**: 변경된 파일의 `#include` 검사 → 신규 위반 발견 시 Stage 2 복귀
5. **Roundtrip** (저장 경로 변경 시): 테스트 PDF 열기 → 저장 → `qpdf --check` → 다시 열기

**재시도 정책**: 동일 에러 최대 3회 재시도. 초과 시 사용자에게 보고 및 대안 제시.

**Verify Report**:
- Build: Pass / Fail (에러 요약, 신규 경고 수)
- Test: Pass (N개 통과) / Fail (실패 테스트 목록)
- Format: Pass / 수정 파일 목록
- Layer: 위반 없음 / 신규 위반 (상세)
- Roundtrip: Pass / Fail / 해당 없음

### Stage 4: QA Test Cases (메인 에이전트 직접 수행)

Verify 통과 후, 개발자가 실제 앱에서 직접 테스트할 수 있는 QA 시나리오를 작성한다.

작성 기준:
1. **설계서**(`02-design.md`)의 기능 요구사항에서 테스트 대상 도출
2. **변경된 코드**를 분석하여 영향받는 UI 흐름·CLI 동작 식별
3. **기존 기능 회귀** 가능성이 있는 영역 포함
4. **타 뷰어 호환**: 저장 결과를 Adobe Reader·Chrome·Edge 에서 열어 보는 항목 포함 (저장 관련 변경 시)

테스트 케이스 분류:
- **Core**: 구현된 기능의 핵심 동작 검증 (반드시 통과해야 함)
- **Edge**: 손상 PDF, 암호화 PDF, 0쪽/1,000쪽 문서, 서브셋 폰트, 한글 문서
- **Regression**: 기존 기능 중 영향받을 수 있는 항목
- **Compat/Perf**: 타 뷰어 표시 일치, 첫 화면 1초, 스크롤 끊김, 고해상도 DPI

각 테스트 케이스 포맷:
```markdown
### TC-N. [테스트 제목]
| 항목 | 내용 |
|------|------|
| **분류** | Core / Edge / Regression / Compat/Perf |
| **전제조건** | 테스트 시작 전 필요한 상태 (사용할 testdata 파일) |
| **절차** | 1. → 2. → 3. (구체적 조작 단계) |
| **기대결과** | 정상 동작 시 예상되는 화면/동작 |
```

**Output**: `.claude/workflow/{branch}/03-qa.md`에 저장

### Stage 간 흐름

```
Validate ──확인──▶ Implement ──코드──▶ Verify ──통과──▶ QA Cases
                       ▲                  │
                       └──── 실패 시 복귀 ─┘
```

## 준수사항
- CLAUDE.md의 핵심 설계 원칙, Coding Guidelines 엄수
- 기존 RAII 래퍼·Command 베이스·유틸 재사용
- Layer Dependency Rules 위반 금지
- 설계서(`02-design.md`)의 구현 계획을 충실히 따를 것
- 새 외부 의존성(`vcpkg.json`) 추가는 사용자 확인 후

## 완료 후 승인 절차 (필수 실행)

아래 절차를 **반드시** 순서대로 실행한다. 생략 금지.

### Step 1: 완료 보고
Verify Report + QA 테스트 케이스 요약 + 변경 파일 목록을 텍스트로 출력한다.

### Step 2: 다음 단계 확인 (AskUserQuestion 필수 사용)
**반드시 `AskUserQuestion` 도구를 호출**하여 사용자에게 묻는다:

- question: "다음 단계를 선택해주세요."
- options: ["다음 단계 진행 (/test)", "수정 후 재실행", "여기서 중단"]

### Step 3: 응답에 따른 분기
- **"다음 단계 진행"** → `Skill` 도구로 `/test` 명령을 **즉시** 실행한다
- **"수정 후 재실행"** → 사용자 피드백을 반영하여 현재 단계 재수행
- **"여기서 중단"** → 워크플로우 중단 안내
