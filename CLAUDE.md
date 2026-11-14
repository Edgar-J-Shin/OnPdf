# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# Role & Persona
You are a Senior C++ Systems Developer with deep PDF (PDFium / qpdf) and Qt experience.
Your goal is to write Safe, Testable, and Maintainable modern C++ (C++23, std::expected).

# Project
**OnPdf** — PDFium 기반의 가볍고 빠른 Windows·macOS용 오픈소스 PDF 뷰어·편집기.
Repo: `Edgar-J-Shin/OnPdf` · 라이선스 Apache 2.0 · git flow (기본 브랜치 `develop`, 릴리스 `main`)
1인 개발(주 10~15시간). 전체 계획서: [pdf-editor-plan.md](pdf-editor-plan.md) — 범위·로드맵·위험 대응은 이 문서가 기준이다.

**현재 단계**: M1 뷰어 (M0 완료 2026-11-14: CI·첫 쪽 표시·테스트 PDF 29개). 연속 스크롤·타일 캐시(#10) 위에 검색·썸네일·목차·텍스트 선택·인쇄를 쌓는다.

| 마일스톤 | 핵심 | 완료 기준 |
|---|---|---|
| M0 기반 | CMake, PDFium 링크, 첫 쪽 표시 | CI 빌드 통과 |
| M1 뷰어 | 연속 스크롤, 타일 캐시, 검색, 썸네일, 목차 | 1,000쪽 스크롤 끊김 없음 → v0.1 |
| M2 주석·페이지 | 주석, 페이지 조작, 저장, 실행 취소 | Adobe Reader 에서 동일 표시 → v0.2 |
| M3 객체 편집 | 텍스트·이미지 추가, 폼, 폰트 임베딩 | 한글 텍스트 타 뷰어 정상 → v0.3 |
| M4 텍스트 수정 | 줄 단위 수정, 블록 인식, 폰트 대체 | 50개 문서 자연스러움 → v0.5 |
| M5 마무리 | 성능, 설치 파일, 다국어, 문서화 | 성공 기준 전부 → v1.0 |

# Tech Stack
- Language: C++23 (std::expected 사용), CMake ≥ 3.25 + vcpkg(manifest 모드), Ninja
- 배포 타깃: Windows x64(MSVC 2022) 와 macOS 13+(Apple Clang, arm64·x64). 개발 머신은 macOS. Linux 는 `core`·`cli`·`tests` CI 빌드만. 플랫폼 분기는 최소화하고 Qt 에 맡긴다. 설치 파일: Windows 설치기 + macOS DMG(공증) — M5
- PDF 엔진: PDFium (`bblanchon/pdfium-binaries` 사전 빌드, Chromium 브랜치 번호로 버전 고정 — `third_party/` 스크립트)
- 구조 작업: qpdf (병합·분할·암호화·검사·복구)
- UI: Qt 6 Widgets, **동적 링크**(LGPL 준수)
- 글자 배치: HarfBuzz + FreeType
- 테스트: GoogleTest + 렌더링 이미지 비교
- CI: GitHub Actions — `windows-latest`·`macos-latest` 는 Qt 앱·app 테스트까지(`dev-app`), `ubuntu-latest` 는 core(`dev`) + testdata 결정성 검사
- 설치 파일: WiX 또는 Inno Setup (M5)

# Build Commands

> 사전 준비: `cmake`·`ninja`(brew), vcpkg 는 `~/vcpkg` 에 **full clone**(shallow 금지 — 버전 해석 실패) 후 `export VCPKG_ROOT=$HOME/vcpkg`.
> CI 와 로컬 모두 `CMakePresets.json` 의 preset 이름을 쓴다: `dev`(Debug), `release`, `dev-app`(Qt 포함).

```bash
export VCPKG_ROOT=$HOME/vcpkg
export QT_ROOT=$(brew --prefix qt)      # dev-app preset 이 CMAKE_PREFIX_PATH 로 넘긴다 (Homebrew qt 6.12)
python3 third_party/fetch_pdfium.py     # PDFium 사전 빌드 받기 (버전: third_party/pdfium-version.txt)
cmake --preset dev                      # configure (vcpkg manifest 설치 포함)
cmake --build --preset dev              # build all → build/dev/bin/
ctest --preset dev                      # unit + rendering regression
./build/dev/bin/onpdf_cli info in.pdf
./build/dev/bin/onpdf_cli render in.pdf out.png --page 0 --scale 2
./build/dev/bin/onpdf_cli tiles in.pdf out-dir --page 0 --scale 2   # 타일 PNG + stitched.png + full.png
./build/dev/bin/onpdf_cli text in.pdf --page 0                       # 글자·줄·링크 수 + 추출 텍스트
ONPDF_UPDATE_REFERENCES=1 ./build/dev/bin/onpdf_render_tests   # 렌더링 기준 이미지 갱신
cmake --preset dev-app && cmake --build --preset dev-app        # Qt 앱 포함 빌드
./build/dev-app/bin/onpdf_app testdata/simple.pdf              # 앱 실행
ctest --preset dev-app                                         # app 테스트(offscreen) 포함
```

- 테스트 PDF: `python3 testdata/gen/make_all.py` 로 전부 생성(표준 라이브러리만, 결정적). `--check` 가 CI 에서 커밋본과 비교. 파일별 기대값은 `testdata/manifest.tsv`, 절차는 `testdata/README.md`
- `core` 는 `stb` 로 PNG 를 읽고 쓴다. 새 의존성은 `vcpkg.json` 에 추가하고 `builtin-baseline` 을 유지한다
- 빌드 산출물은 `build/<preset>/` 아래에만 생성한다 (소스 트리 내 빌드 금지). 실행 파일과 PDFium 공유 라이브러리는 `build/<preset>/bin/`
- PDFium/qpdf 바이너리는 저장소에 커밋하지 않는다 — `third_party/` 스크립트로 받아온다
- 테스트 PDF 는 라이선스가 확인된 파일만 `testdata/` 에 넣고, `manifest.tsv` 에 기대값·출처·라이선스를 적는다 (README 표는 자동 생성). 한글 파일명(`한글 파일명.pdf`)이 있으므로 경로는 항상 UTF-8 `std::filesystem::path` 로 다룬다

# Architecture

```text
app/          Qt UI (Qt 의존). onpdf_app_lib(UiDispatcher·DocumentSession·qt_image·DocumentTileSource·DocumentView) + onpdf_app(MainWindow·main)
edit/         Command, 실행 취소 스택, 선택 모델, 텍스트 블록 모델, 폰트 매니저
core/         Document, Page, 렌더 캐시(타일 LRU), 텍스트 인덱스       (Qt 의존 금지)
core/engine/  PdfiumLibrary(init RAII) · PdfiumDocument(열기·크기·렌더·load_text) · TextPage(FPDFText/FPDFLink 추출, 좌표 변환) · pdfium_handles
              PdfiumWorker(전용 스레드 + High/Normal/Low 큐 + 취소) · DocumentHandle(워커 쪽 문서의 스레드 안전 핸들), QpdfTools(M2)
core/image/   png_io (stb 기반 PNG 읽기/쓰기, BGRA↔RGBA)
core/view/    PageLayout(세로 연속 레이아웃·좌표 변환) · tile.h(TileKey, 512px, 1/64 배율 양자화) · TileCache(바이트 상한 LRU)
              TileScheduler(뷰포트→필요 타일, High/Low 요청·취소·실패 집합) · TileSource(SyncTileSource=CLI·테스트)
              PageTextIndex(쪽 텍스트 불변 스냅샷: 글자·줄·링크, hit-test, 선택 사각형) · TextSelection(다중 쪽 선택)   (메인 스레드 전용, Qt 금지)
core/platform/ path.h (UTF-8 ↔ std::filesystem::path)
cli/          렌더·병합·검증용 명령줄 도구                            (core/edit 만 사용)
tests/        단위 테스트, 렌더링 회귀, 왕복 테스트
testdata/     테스트 PDF + 기준 PNG
third_party/  PDFium 바이너리 다운로드 스크립트
docs/         설계 문서, 기여 가이드
```

### Layer Dependency Rules

```text
core/   → edit/, app/, cli/ include 금지. Qt 헤더 include 금지
edit/   → core/ 만 include. Qt 헤더 include 금지
app/    → core/, edit/ include 가능
cli/    → core/, edit/ include 가능. Qt 금지
PDFium / qpdf 헤더(fpdf*.h, qpdf/*)는 core/engine/ 안에서만 include
```

Qt 가 필요한 기능이 core 에 들어가려 하면, core 에 순수 C++ 인터페이스를 두고 app 에서 구현한다.

### 핵심 설계 원칙 (위반 금지)
1. **core 는 UI 를 모른다.** CLI 와 테스트에서 core/edit 를 그대로 쓴다.
2. **PDFium 호출은 전용 워커 스레드 한 곳에서만.** 앱은 `PdfiumWorker` 하나를 만들고 `DocumentHandle` 로만 문서를 다룬다. 콜백은 워커 스레드에서 호출되므로 앱은 반드시 `UiDispatcher::post_to` 로 메인 스레드에 넘긴 뒤 처리한다(수신자는 `QPointer` 로 보호). 워커 콜백 람다는 위젯 포인터를 직접 잡지 않는다. CLI·테스트처럼 단일 스레드면 `PdfiumLibrary` 를 직접 만들어도 되지만 한 프로세스에서 둘을 섞지 않는다. 디버그 빌드는 `PdfiumLibrary::assert_current_thread()` 로 워커 밖 호출을 잡는다.
3. **모든 문서 변경은 Command 로.** `execute()` / `undo()` 를 가진 명령 객체 외의 경로로 문서를 바꾸지 않는다.
4. **핸들은 RAII.** `FPDF_DOCUMENT`, `FPDF_PAGE`, `FPDF_TEXTPAGE`, `FPDF_BITMAP` 등은 `std::unique_ptr` + 커스텀 삭제자로만 보유한다. raw 핸들을 반환하는 public API 금지.
5. **원본 파일은 건드리지 않는다.** 저장은 임시 파일 → 다시 열어 검증(페이지 수·렌더링·qpdf check) → 교체. 서명된 문서는 증분 저장.
6. **JavaScript 실행은 기본 비활성화.** 악성 PDF 대응. 외부 링크는 `view::is_safe_uri`(http/https/mailto) 를 통과한 것만 사용자 확인 후 연다. `file:`·`javascript:`·`data:` 와 Launch/RemoteGoTo 액션은 무시한다.

### 렌더링 (구현: core/view + app/DocumentView)
- 쪽을 512×512 타일로 나눠 `PdfiumDocument::render_region`(`FPDF_RenderPageBitmapWithMatrix`, 회전 0 기준 — `page_size` 가 /Rotate 반영) 으로 그리고 `TileCache`(기본 256MB LRU) 에 보관
- `TileScheduler::update(viewport)` 가 보이는 타일·쪽 미리보기(배율 1/8)를 kHigh, 앞뒤 1쪽(타일 ≤32개일 때)을 kLow 로 요청하고 밀려난 요청은 `TaskHandle::cancel`
- 뷰는 쪽 배경 → 미리보기 → 이전 배율 타일(늘려 그림) → 현재 배율 타일 순으로 그려 확대·스크롤 중 빈 화면이 없다
- 배율은 `quantize_scale(zoom × dpr)` 로 1/64 단위. 맞춤 배율은 내림(`fit_*_scale_q`) 이라 문서가 뷰포트를 넘지 않는다
- 타일 경계는 AGG 클리핑 때문에 전체 렌더와 픽셀 단위로 완전히 같지는 않다(0.03% 수준). `tile_stitch_test` 가 0.1% 상한으로 검증

# Coding Guidelines
1. **Safety First**: 모든 PDFium 반환값(nullptr, FALSE, 음수)을 검사한다. 파일 I/O 와 외부 라이브러리 호출은 `std::expected` 또는 명시적 에러 타입으로 전파하고, 예외는 경계(UI·CLI)에서만 잡는다.
2. **Ownership 명확히**: raw `new`/`delete` 금지. 소유권은 `unique_ptr`, 공유는 꼭 필요할 때만 `shared_ptr`, 비소유 참조는 참조 또는 raw pointer(nullable 명시).
3. **스레드**: PDFium 워커 외 스레드에서 PDFium 객체 접근 금지. 공유 상태는 `std::mutex` 또는 메시지 전달로 보호. 데이터 레이스는 Critical 이슈로 취급.
4. **텍스트는 UTF-8**: 내부 문자열은 UTF-8 `std::string`. PDFium 의 UTF-16LE 와는 core/engine 경계에서만 변환. 한글(CID 폰트) 경로는 항상 테스트를 함께 만든다. 글자 좌표는 PDFium 사용자 좌표(좌하단, 회전 전)가 아니라 `TextPage` 가 `FPDF_PageToDevice`(×1000 트릭)로 바꾼 "쪽 포인트"(좌상단, 회전 반영)를 쓴다.
5. **플랫폼 분기 최소화**: `#ifdef _WIN32` 는 `core/platform/`, `app/`, `cli/main.cpp`(wmain) 에만. 경로는 `std::filesystem::path` 이되 **UTF-8 문자열에서 만들 때는 반드시 `onpdf::path_from_utf8`**(`core/platform/path.h`) 를 쓴다 — Windows 는 `path(std::string)` 을 코드 페이지로 해석해 한글 경로가 깨진다. 오류 메시지 등 역변환은 `path_to_utf8`.
6. **스타일**: `.clang-format` 준수(생기면). 클래스 `PascalCase`, 함수·변수 `snake_case`, 멤버 `name_`, 상수 `kName`. 헤더는 `#pragma once`. 주석·문서는 한국어, 식별자·커밋 메시지 제목은 영어.
7. **i18n**: 사용자 노출 문자열은 `tr()` 로 감싸고 `app/i18n/` 에 ko/en 둘 다 추가. 하드코딩 금지.
8. **Simplicity**: 보일러플레이트 최소화, 기존 패턴 우선. 새 의존성은 `vcpkg.json` 추가 전에 상의.

# Testing Strategy
- **단위 테스트** (`tests/unit`): core/edit. 모든 Command 는 실행 → 취소 → 재실행 후 문서 상태 비교 테스트 필수.
- **렌더링 회귀** (`tests/render`): 테스트 PDF → PNG → 기준 이미지와 픽셀 비교. 렌더링·편집 기능 변경 시 반드시 갱신. 테스트는 `PdfiumLibrary::Options::use_system_fonts=false` 로 내장 폰트만 써서 플랫폼 간 결과를 같게 한다 (시스템 폰트 의존 테스트는 별도 표시).
- **왕복 테스트** (`tests/roundtrip`): 열기 → 저장 → 다시 열기 후 페이지 수·텍스트·렌더링 일치 + `qpdf --check`.
- 한글 문서, 스캔본, 폼, 암호화 파일을 테스트 집합에 항상 포함한다. `tests/unit/testdata_manifest_test.cpp` 가 `manifest.tsv` 의 전 파일을 열어 검증하므로 새 PDF 는 manifest 한 줄로 테스트에 들어간다.
- CI: 푸시마다 빌드 + 단위 테스트, PR 마다 렌더링 회귀까지.

# Task Complexity Routing

| 규모 | 기준 | 접근 방식 |
|------|------|-----------|
| **사소** | 단일 파일, 오타, 주석 | 바로 수정 → 빌드·테스트 검증 |
| **중간** | 2~5개 파일, 기존 패턴 따름 | Explore 로 영향 확인 → 구현 → 빌드·테스트 검증 |
| **대형** | 새 기능, 다중 레이어, 아키텍처 변경 | `/planning` → `/design` → `/develop` → `/test` 워크플로우 |
| **불확실** | PDFium API 동작·성능·폰트 임베딩 등 검증 필요 | `/spike` 로 2주 이내 사전 검증 후 결정 |

# Git / Release (git flow)
- `develop`: 통합·기본 브랜치. 모든 작업 PR 의 base. 직접 push 금지
- `main`: 릴리스 브랜치. `release/x.y.z`·`hotfix/x.y.z` 만 머지하고 태그 `vx.y.z` 를 단다. 직접 push 금지
- **작업 시작 절차**: ① GitHub 이슈 생성(제목 접두 `M0:`~`M5:`, 마일스톤 지정, 필요 시 라벨) → ② `develop` 에서 브랜치 분기 → ③ 구현 → ④ PR(base `develop`, 본문에 `Closes #N`) → ⑤ CI 통과 후 머지 커밋(`gh pr merge --merge`)
- 브랜치 이름: `feature/<이슈번호>-<slug>`, `fix/<이슈번호>-<slug>`, `chore/<이슈번호>-<slug>`, `spike/<이슈번호>-<slug>`, `release/x.y.z`, `hotfix/x.y.z` (slug 는 영문 소문자·하이픈)
- 릴리스: `develop` → `release/x.y.z` 분기 → 버전·CHANGELOG 정리 → `main` 머지 → 태그 → `main` 을 `develop` 에 백머지. GitHub Actions 가 태그에서 설치 파일·무설치 압축본 생성
- 커밋 메시지: 제목 영어 명령형 72자 이내, 본문 한국어 가능
- 변경 기록은 `CHANGELOG.md`, 제3자 고지는 `THIRD_PARTY_NOTICES.md` (PDFium, qpdf, Qt, FreeType, HarfBuzz)
- 커밋 금지: `build/`, `vcpkg_installed/`, PDFium/qpdf 바이너리, 라이선스 불명확한 PDF, 코드 서명 인증서

# Agent Commands
워크플로우 슬래시 명령. 산출물 경로: `.claude/workflow/{branch}/` (로컬 전용, gitignore).
`/planning` 과 `/spike` 는 시작할 때 이슈가 없으면 만들고 `develop` 에서 이슈 번호 브랜치를 분기한다.
- `/planning` → PRD → `01-plan.md`
- `/design` → 설계 + 구현 계획 → `02-design.md`
- `/develop` → Validate → Implement → Verify → QA Cases → `03-qa.md`
- `/test` → 코드 리뷰 → `04-review.md`
- `/spike` → 기술 검증 → `01-spike.md`
