# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# Role & Persona
You are a Senior C++ Systems Developer with deep PDF (PDFium / qpdf) and Qt experience.
Your goal is to write Safe, Testable, and Maintainable modern C++ (C++23, std::expected).

# Project
**OnPdf** — PDFium 기반의 가볍고 빠른 Windows·macOS용 오픈소스 PDF 뷰어·편집기.
Repo: `Edgar-J-Shin/OnPdf` · 라이선스 Apache 2.0 · git flow (기본 브랜치 `develop`, 릴리스 `main`)
1인 개발(주 10~15시간). 전체 계획서: [pdf-editor-plan.md](pdf-editor-plan.md) — 범위·로드맵·위험 대응은 이 문서가 기준이다.

**현재 단계**: M2 완료 → v0.2.0 릴리스(2026-11-19: 주석·쪽 조작·병합·분할·저장·실행 취소). 다음은 M3 객체 편집(텍스트·이미지 추가, 폼, 폰트 임베딩). 남은 수동 QA: Windows 실기기, Adobe Reader·Chrome·Edge 에서 주석 동일 표시

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
- 구조 작업: qpdf 12 (vcpkg, 라이브러리만·내장 암호 공급자) — 병합·분할·구조 검사. 헤더는 `core/src/engine/qpdf_tools.cpp` 에서만. 암호화 설정·복구 저장은 후속
- UI: Qt 6 Widgets, **동적 링크**(LGPL 준수)
- 글자 배치·서브셋: HarfBuzz(vcpkg, `default-features: false`, `harfbuzz::harfbuzz-subset`) — **edit 에만** 링크. FreeType 은 쓰지 않는다(PDFium 공유 라이브러리가 `FT_*` 154개를 내보내 Linux 에서 충돌 위험). 번들 글꼴 Noto Sans KR(OFL)은 `third_party/fetch_fonts.py` 로 받아 앱 빌드가 `bin/fonts/` 로 복사한다(저장소에 없음)
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
python3 third_party/fetch_fonts.py      # 번들 글꼴 Noto Sans KR 받기 (없으면 앱의 텍스트 추가 도구가 비활성; 테스트는 저장소 서브셋을 쓴다)
# vcpkg 의 harfbuzz 는 meson 빌드라 python ≥ 3.10 이 PATH 에 있어야 한다 (macOS 기본 3.9 → brew python 을 앞에)
cmake --preset dev                      # configure (vcpkg manifest 설치 포함)
cmake --build --preset dev              # build all → build/dev/bin/
ctest --preset dev                      # unit + rendering regression + roundtrip(열기→저장→다시 열기, qpdf 있으면 --check)
./build/dev/bin/onpdf_cli info in.pdf                                # 쪽 크기 + 인쇄 권한·문서 인쇄 기본값
./build/dev/bin/onpdf_cli render in.pdf out.png --page 0 --scale 2 [--print]   # --print: 인쇄용 렌더(FPDF_PRINTING)
./build/dev/bin/onpdf_cli tiles in.pdf out-dir --page 0 --scale 2   # 타일 PNG + stitched.png + full.png
./build/dev/bin/onpdf_cli text in.pdf --page 0                       # 글자·줄·링크 수 + 추출 텍스트
./build/dev/bin/onpdf_cli search in.pdf "query" [--case] [--word]    # 쪽별 결과 + 문맥
./build/dev/bin/onpdf_cli outline in.pdf                             # 목차 트리 (깊이 들여쓰기, [p.N]/[uri])
./build/dev/bin/onpdf_cli merge out.pdf a.pdf b.pdf                  # 병합 (첫 파일의 목차·폼 유지, qpdf)
./build/dev/bin/onpdf_cli split in.pdf out-dir --every 3             # 분할 (--ranges "1-3,5" | --each, --pattern NAME-%d.pdf)
./build/dev/bin/onpdf_cli check in.pdf                               # qpdf 구조 검사 (경고 출력, 오류면 1)
ONPDF_UPDATE_REFERENCES=1 ./build/dev/bin/onpdf_render_tests   # 렌더링 기준 이미지 갱신
cmake --preset dev-app && cmake --build --preset dev-app        # Qt 앱 포함 빌드
./build/dev-app/bin/onpdf_app testdata/simple.pdf              # 앱 실행
ctest --preset dev-app                                         # app 테스트(offscreen) 포함
```

- 테스트 PDF: `python3 testdata/gen/make_all.py` 로 전부 생성(표준 라이브러리만, 결정적). `--check` 가 CI 에서 커밋본과 비교. 파일별 기대값은 `testdata/manifest.tsv`, 절차는 `testdata/README.md`. 왕복 테스트의 구조 검사는 qpdf **라이브러리**(`QpdfTools::check`)라 외부 명령이 없어도 돈다
- `core` 는 `stb` 로 PNG 를 읽고 쓴다. 새 의존성은 `vcpkg.json` 에 추가하고 `builtin-baseline` 을 유지한다
- 빌드 산출물은 `build/<preset>/` 아래에만 생성한다 (소스 트리 내 빌드 금지). 실행 파일과 PDFium 공유 라이브러리는 `build/<preset>/bin/`
- PDFium/qpdf 바이너리는 저장소에 커밋하지 않는다 — `third_party/` 스크립트로 받아온다
- 테스트 PDF 는 라이선스가 확인된 파일만 `testdata/` 에 넣고, `manifest.tsv` 에 기대값·출처·라이선스를 적는다 (README 표는 자동 생성). 한글 파일명(`한글 파일명.pdf`)이 있으므로 경로는 항상 UTF-8 `std::filesystem::path` 로 다룬다

# Architecture

```text
app/          Qt UI (Qt 의존). onpdf_app_lib(UiDispatcher·DocumentSession·qt_image·DocumentTileSource·DocumentView·SearchBar·ThumbnailPanel·OutlinePanel·PrintJob·EditController·AnnotationDialog·FormFieldEditor·StructureJob·MainWindow) + onpdf_app(main 만)
edit/         Command(execute/undo/discard, EditableDocument 로만 문서 변경)·UndoStack(저장 지점, push 가 밀려난 명령 반환)
              RotatePageCommand(여러 쪽)·DeletePagesCommand(스냅샷으로 취소)·MovePagesCommand(역순열 취소)·InsertPagesCommand(파일 스냅샷)
              annotation_commands(AddAnnotations·RemoveAnnotation·SetAnnotationColor·SetAnnotationContents — /NM 과 값만 보관)
              font_face(HarfBuzz pimpl: 배치·글리프 없음·서브셋(GID 재배치)·ToUnicode 생성)·text_layout(줄·기준선)·AddTextCommand(객체 id 보관)
              form_commands(SetFormText·SetFormChecked·SetFormChoice — FieldRef 와 이전 값만 보관)
              page_ops(normalize·validate_move·simulate_move·apply_order·structural_change). 선택·텍스트 블록·폰트(M3)
core/         Document, Page, 렌더 캐시(타일 LRU), 텍스트 인덱스       (Qt 의존 금지)
core/engine/  PdfiumLibrary(init RAII) · PdfiumDocument(열기·크기·렌더·load_text·load_outline·resolve_location·print_preferences·회전·save_to — EditableDocument 구현) · TextPage(FPDFText/FPDFLink 추출, 좌표 변환) · dest(목적지·URI·UTF-16 변환 공용, 내부) · pdfium_handles
              PdfiumWorker(전용 스레드 + High/Normal/Low 큐 + 취소) · DocumentHandle(워커 쪽 문서의 스레드 안전 핸들)
              QpdfTools(qpdf C 작업 API: merge(첫 입력이 주 입력 — 그 목차·폼 보존)·extract·check·page_count·allows_extraction — PDFium 과 무관, 어느 스레드든)
core/engine/  EditableDocument(PDFium 없는 편집 인터페이스 — edit/ 가 씀: 회전·삭제·이동·스냅샷(SnapshotId)·삽입·주석)·ChangeSet(structural+all_sizes)
              save(write_files_safely: 생산자 무관 "임시 → 검증 → 교체" 골격, 여러 대상은 전부-또는-없음; save_document_safely 는 그 위) · page_ranges("1-3,5,8-" 파서·N쪽마다·파일 이름 패턴)
              annotation.h(AnnotationInfo·AnnotationSpec(쪽 포인트)·AnnotationSnapshot(사용자 좌표 원시값+AP) — PDFium 없음)
              save(save_document_safely: 임시→검증→rename)
core/platform/ fs_sync(flush_file_to_disk·replace_file·flush_directory — 저장의 플랫폼 분기는 여기만)
core/image/   png_io (stb 기반 PNG 읽기/쓰기, BGRA↔RGBA)
core/view/    PageLayout(세로 연속 레이아웃·좌표 변환) · tile.h(TileKey, 512px, 1/64 배율 양자화) · TileCache(바이트 상한 LRU)
              TileScheduler(뷰포트→필요 타일, High/Low 요청·취소·실패 집합) · TileSource(SyncTileSource=CLI·테스트)
              PageTextIndex(쪽 텍스트 불변 스냅샷: 글자·줄·링크, hit-test, 선택 사각형 — 생성은 워커, 조회는 메인) · TextSelection(다중 쪽 선택)
              print_layout(인쇄: 쪽 범위→순서·매수, 용지 배치 맞춤/실제/큰 쪽만 축소·자동 회전, 렌더 DPI 300·저품질 150·16M px 상한, PrintPreferences)
              Outline(목차 불변 스냅샷: 전위 평탄화, 깊이 32·10만 개 상한, 순환 차단, item_at 현재 항목)
              search(SearchSession: 현재 쪽부터 순환 검색, 동시 4쪽·kNormal, 세대·10만 상한 — 실행기 주입, 메인 스레드; map_source_range)   (Qt 금지)
core/platform/ path.h (UTF-8 ↔ std::filesystem::path)
cli/          렌더·병합·검증용 명령줄 도구                            (core/edit 만 사용)
tests/spike/  기술 검증 실험 (ONPDF_BUILD_SPIKES=ON 에서만 빌드, ctest 미등록, PDFium 헤더 직접 사용 허용)
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
PDFium / qpdf 헤더(fpdf*.h, qpdf/*)는 core/engine/ 안에서만 include (qpdf 는 `qpdf_tools.cpp` 한 파일)
```

Qt 가 필요한 기능이 core 에 들어가려 하면, core 에 순수 C++ 인터페이스를 두고 app 에서 구현한다.

### 핵심 설계 원칙 (위반 금지)
1. **core 는 UI 를 모른다.** CLI 와 테스트에서 core/edit 를 그대로 쓴다.
2. **PDFium 호출은 전용 워커 스레드 한 곳에서만.** 앱은 `PdfiumWorker` 하나를 만들고 `DocumentHandle` 로만 문서를 다룬다. 콜백은 워커 스레드에서 호출되므로 앱은 반드시 `UiDispatcher::post_to` 로 메인 스레드에 넘긴 뒤 처리한다(수신자는 `QPointer` 로 보호). 워커 콜백 람다는 위젯 포인터를 직접 잡지 않는다. CLI·테스트처럼 단일 스레드면 `PdfiumLibrary` 를 직접 만들어도 되지만 한 프로세스에서 둘을 섞지 않는다. 디버그 빌드는 `PdfiumLibrary::assert_current_thread()` 로 워커 밖 호출을 잡는다.
3. **모든 문서 변경은 Command 로.** `execute()` / `undo()` 를 가진 명령 객체 외의 경로로 문서를 바꾸지 않는다.
4. **핸들은 RAII.** `FPDF_DOCUMENT`, `FPDF_PAGE`, `FPDF_TEXTPAGE`, `FPDF_BITMAP` 등은 `std::unique_ptr` + 커스텀 삭제자로만 보유한다. raw 핸들을 반환하는 public API 금지.
5. **원본 파일은 건드리지 않는다.** 저장은 임시 파일 → 다시 열어 검증(페이지 수·렌더링·qpdf check) → 교체. 서명된 문서는 증분 저장.
6. **JavaScript 실행은 기본 비활성화.** 악성 PDF 대응. 외부 링크는 `view::is_safe_uri`(http/https/mailto) 를 통과한 것만 사용자 확인 후 연다. `file:`·`javascript:`·`data:` 와 Launch/RemoteGoTo 액션은 무시한다.

### 편집·저장 (구현: edit + core/engine/save + app/EditController)
- `edit/` 는 PDFium 헤더를 끌어오는 `pdfium_document.h` 를 쓰지 않고 `engine::EditableDocument`(PDFium 없음)로만 문서를 바꾼다
- 실행: `EditController::run(cmd)` → `DocumentHandle::modify`(워커, kHigh) → 결과 `ChangeSet` 을 메인에서 `apply_change`(문서 정보의 쪽 크기 — 메인 스레드만 고친다) → `UndoStack` → `document_changed` → `DocumentView::invalidate_pages`(타일·텍스트 버림, 스케줄러 재생성으로 이전 모양의 늦은 타일 폐기, 크기가 바뀌면 레이아웃 재구성)·`ThumbnailPanel::invalidate_pages`·검색 재실행. 한 번에 하나(busy)
- 저장: `DocumentHandle::save` → 워커 `save_document_safely`: 심볼릭 링크면 실제 파일로(링크 유지) → 기존 파일은 일반 파일·쓰기 가능(읽기 전용·잠금이면 `kNotWritable` 로 거절) 확인 → 같은 폴더 임시 파일(64비트 난수 이름) → 같은 암호로 다시 열어 쪽 수·크기·모든 쪽 로드(증분이면 원본 바이트 앞부분 일치) 검증 → 원본 권한 복사 → `platform::flush_file_to_disk`(macOS F_FULLFSYNC·fsync / Windows FlushFileBuffers) → `platform::replace_file`(rename / MoveFileExW+WRITE_THROUGH) → 폴더 동기화. 실패하면 원본 불변·임시 삭제. 서명 문서(`signature_count > 0`)는 증분 저장 강제, 암호화는 유지
- 앱: 수정 표시(`setWindowModified`, 제목 `"<파일>[*] — OnPdf"`), 닫기·종료(→`close()`)·다른 문서 열기 전 `maybe_save`(저장/저장 안 함/취소, 그다음 인쇄 확인). 편집 종류별 권한(`DocumentInfo::permissions`: 쪽 회전·삭제·삽입 = `assemble`(R≥3 비트 11, R2 비트 4), 주석 = `annotate`(비트 6))이 없거나 인쇄·저장 중이면 해당 편집 비활성
- 모든 Command 는 실행 → 취소 → 재실행 후 문서 상태 비교 테스트(가짜 `EditableDocument`(`tests/fake_document.h`) + 실제 PDFium) 필수. 원래 경로로 저장하는 테스트는 testdata 사본(`ScopedTempDir`)을 연다
- **쪽 구조 편집(삭제·이동·삽입, #33):** 결과는 `ChangeSet::structural` + `all_sizes` → `apply_change` 가 쪽 수·크기 목록을 통째로 교체 → `DocumentView::rebuild_pages`(캐시·텍스트·선택 전부 폐기, 레이아웃 재구성, 현재 쪽 클램프)·`ThumbnailPanel::reset_pages`(세대↑, 행 재설정)·쪽 번호 스핀·목차 재로드(`load_outline(false)`, 탭 유지)·검색 재실행. 저장·이동 결과 폐기는 `document_generation_`(열 때만 증가), 목차는 `outline_generation_`
- **스냅샷은 문서가 소유한다.** 삭제 취소용 쪽 사본·삽입 소스는 `PdfiumDocument::snapshots_`(워커 전용 PDFium 문서)에 있고 Command 는 `SnapshotId` 만 쥔다 — Command 는 메인 스레드(실행 취소 스택)에서 소멸하므로 PDFium 핸들을 직접 쥐면 안 된다. 스택에서 밀려난 Command 는 `EditController` 가 워커에서 `discard` 를 불러 해제한다. `FPDF_MovePages` 는 실패 시 문서가 불확정이므로 `validate_move` 로 호출 전에 전부 검증한다. 전체 삭제는 금지(쪽 ≥ 1)
- **주석(#35):** 좌표는 뷰·Command 는 쪽 포인트, PDF 는 사용자 좌표 — 변환은 `detail::PageSpace::to_user/quad_of/user_rect_of`(engine 안)에서만. 마크업 QuadPoints 는 선택 사각형(`selection_rects`) 꼭짓점을 변환해 좌상·우상·좌하·우하 순서. **새 주석은 AP 없이 만든다**(PDFium 이 렌더 때 AP 를 만들어 사전에 저장한다). 렌더되지 않은 채 저장하면 AP 가 빠져 다른 뷰어가 다르게 그리므로 **`save_to` 가 먼저 `ensure_annotation_appearances()`** 로 AP 없는 OnPdf 주석(/NM `onpdf-…`)이 있는 쪽만 1×1 렌더한다(#53). 이 렌더(`CPDF_AnnotList`)는 `FPDFPage_CreateAnnot` 이 /Annots 에 직접 넣은 사전을 간접 객체로도 바꾼다(규격 표 30) — 검증은 `QpdfTools::direct_annotation_count`. AP 가 있으면 `FPDFAnnot_SetColor` 가 실패하므로 색 변경은 `SetAP(Normal, nullptr)` → `SetColor`, 취소는 보관한 이전 AP 로 `SetAP`. 정체성은 /NM(없는 기존 주석은 `"#<index>"` 로 가리키고 편집 때 `onpdf-<16진 16자>` 를 부여). `FPDFPage_RemoveAnnot` 는 되돌릴 수 없어 삭제 취소는 `AnnotationSnapshot`(색·내부색·불투명도·Quad·Ink·플래그·문자열 — **AP 는 보관하지 않는다**: `GetAP/SetAP` 텍스트 복원은 `/Resources` 를 잃어 하이라이트가 글자를 덮는다)으로 재생성하고 PDFium 이 모양을 다시 만든다(같은 NM, /Annots 끝). `snapshot_annotation` 은 AP 를 지우고 색을 읽으므로 삭제·색 변경 직전에만 부른다. Print 플래그 필수(없으면 인쇄 안 됨), 메모는 NoZoom|NoRotate. Popup 은 만들지 않고(FPDF_ANNOT 가 그리지 않음) 메모 내용은 툴팁·`AnnotationDialog`. 링크·위젯·팝업은 `editable=false`. 뷰는 보이는 쪽 ±1 의 `load_annotations`(kLow) 스냅샷으로 hit-test 하고 `invalidate_pages` 가 캐시를 버린다. 도구 모드는 `DocumentView::set_tool`, 도구·단축키는 `permissions.annotate` 로 게이트
- **도형·속성(#39):** 사각형·원은 `AnnotationSpec::rect`(쪽 포인트) + `border_width`(`/Border` 폭) + 선택적 `interior`(`/IC`, 알파는 `color.a` 와 같이 `/CA` 하나). `AnnotationInfo` 가 `border_width`·`opacity`(`/CA` 를 AP 와 무관하게 읽음)·`has_bs` 를 실어 뷰·메뉴가 메인에서 판단한다. **불투명도는 `/CA` = 색 알파로만 쓴다**(`FPDFAnnot_SetNumberValue` 없음) — `SetAnnotationOpacityCommand` 는 `set_annotation_color` 에 알파만 바꿔 호출하고 취소는 색 전체. 두께 `SetAnnotationBorderWidthCommand` 는 도형·펜만; **`/BS` 가 있는 주석(다른 프로그램)은 PDFium 이 `/BS /W` 를 우선하고 `/BS` 를 지울 API 가 없어 삭제 → 같은 NM 으로 `/BS` 없이 재생성**(원래 폭은 읽을 수 없어 취소 때 돌아오지 않음). `FPDFAnnot_GetColor` 는 `/C`·`/IC` 가 없어도 기본 검정으로 성공하므로 `has_color`·`has_interior` 는 `FPDFAnnot_HasKey` 로 판정한다. 앱의 두께·불투명도 드롭다운은 선택이 있으면 그 주석에 Command, 없으면 **새 도형·펜의 기본값만**(마크업·메모는 항상 100%). 편집 뒤 뷰는 같은 NM 을 다시 고른다(`reselect_`; `"#index"` 는 해제)
- **텍스트 추가(#45):** 쪽 객체 API 는 `EditableDocument::add_text_objects/remove_text_objects/text_objects`(`text_object.h`, PDFium 없음). **정체성은 마크 콘텐츠** `OnPdf` + `id` 파라미터(`onpdf-text-<16진 16자>`; 파라미터 값은 UTF-16LE 로 돌아온다). 글꼴은 edit 의 `FontFace`(HarfBuzz)가 배치·서브셋·ToUnicode 를 만들고 엔진은 `FPDFText_LoadCidType2Font` + `FPDFText_SetCharcodes`(CID = 서브셋의 새 GID) 로 넣는다 — `FPDFText_LoadFont` 의 자동 ToUnicode·전체 임베딩 금지(#43). **문서가 서브셋 key(FNV-1a) 로 `FPDF_FONT` 를 캐시**해 재실행·같은 글자 집합은 한 번만 임베딩한다(지운 뒤에도 폰트 사전은 저장에 남는다 — 알려진 제한). 회전 쪽 행렬은 `PageSpace::user_matrix_at`(기준점·기저 벡터 변환, 회전 분기 없음). `FPDFPage_GenerateContent` 는 원본 내용을 보존한다(테스트가 픽셀 단위로 확인). 글꼴에 없는 글자(.notdef)는 Command 가 거절하고 앱은 입력 창에서 경고. 공백/NBSP 처럼 글리프를 공유하는 글자는 같은 글자 집합(같은 폰트 캐시)에서 먼저 나온 코드로 추출된다. 탭·NBSP 는 `shape` 가 공백 글리프로 바꾸고(탭 = 4칸), 클러스터의 모든 코드(NFD 한글·결합 문자)가 ToUnicode 에 들어가며 앱은 입력을 NFC 로 정규화한다. 요청 게이트는 `text_edits_allowed_`(비트 4), 주석의 `annotation_edits_allowed_`(비트 6)와 다르다. 저장 검증은 쪽별 텍스트 객체 id 집합이 같은지 본다. 앱 게이트는 `permissions.modify_contents`(비트 4) + 번들 글꼴; 테스트는 `set_text_font` 로 저장소 서브셋 폰트를 주입한다. 테스트 폰트 글자 집합은 `fetch_font.py` TEXT(NBSP 금지 — 공백과 글리프를 공유해 생성기 ToUnicode 가 바뀐다)
- **텍스트 상자 편집(#47):** 줄 객체는 마크 `box`(상자 id, `onpdf-box-<16진>`)로 묶이고 원문은 마크 **블롭** `text`(UTF-8 — 문자열 파라미터는 비ASCII 를 PDFDocEncoding 으로 오해하고 UTF-16 은 NUL 에서 잘린다)에 있다. `TextObjectInfo` 가 box·text·크기·색·기준선 원점을 돌려주며 `box` 마크가 없는 옛 객체는 `box = id`, 원문은 ToUnicode 추출. 이동은 `move_text_objects(page, ids, delta_pt)` — 쪽 포인트 Δ 를 `user_matrix_at` 기저로 사용자 Δ 로 바꿔 행렬 e,f 만 더한다(회전 쪽에서도 화면 방향). 삭제 취소·수정(`RemoveTextBoxCommand`·`ReplaceTextBoxCommand`)은 `FPDFText_SetText` 를 쓰지 않고 **삭제 + 재생성**(`collect_text_box` → `build_text_objects_spec`, AddText 와 공용): 같은 상자 id, 줄 id 는 순서대로 재사용·모자라면 edit 가 새로 만든다(엔진은 ids 가 전부 있거나 전부 없어야 함). 상자 원점 = 첫 줄 기준선 − ascent(size). **상자 원문 전체(빈 줄 포함)는 첫 줄 객체의 블롭 `box_text`** 에 있다(빈 줄은 객체가 없어 줄 원문만으로는 복원 불가). 삭제 Command 는 지우기 전에 복원 스펙을 먼저 만들어 실패면 거절하고, 수정의 취소도 스펙을 먼저 만든다. 뷰는 `load_text_objects` 스냅샷으로 상자 hit-test(주석이 위에 그려지므로 `annotation_at` 먼저), 선택된 상자 안의 드래그만 이동(4pt 문턱, 쪽 안 클램프), 두 번 클릭 = 수정, 편집 뒤 같은 상자 재선택(`text_box_reselected`). 주석을 고르면 상자 선택은 해제된다(동시 선택 없음). 권한·바쁨은 `DocumentView::set_text_editable` 로 드래그·커서를 막는다. 크기 콤보·색 팔레트는 상자가 선택돼 있으면 `ReplaceTextBoxCommand`, 없으면 기본값. 객체·상자 id 는 `make_text_object_id/make_text_box_id`(`text_object.h`) 하나로. 앱 테스트는 클릭 뒤 드래그 전에 연속 클릭 판정(500ms)을 피해 기다린다
- **이미지·객체 편집(#49):** 쪽 객체 API 는 `page_object.h`(PDFium 없음): `page_objects`(텍스트는 OnPdf 마크, 이미지는 쪽 직속 전부)·`add_image_object`·`move_page_objects`·`set_object_rect`·`mark_object`·`detach_objects/reattach_objects/drop_detached`. **디코딩은 앱(Qt `QImageReader`, `image_import`)**: 픽셀은 직선 알파 BGRA(`QImage::Format_ARGB32`) → `FPDFImageObj_SetBitmap` 이 알파를 `/SMask` 로 넣는다(흰색 합성 금지). EXIF 회전이 없는 JPEG 은 원본 바이트를 `FPDFImageObj_LoadJpegFileInline`(`/DCTDecode`, 재압축 없음 — SOI·픽셀 크기를 먼저 확인), 실패하면 앱이 픽셀로 재시도. PDF·SVG 는 Qt 플러그인이 이미지로 읽어도 이미지가 아니다. 크기 = 1px 1pt, 쪽 폭 80% 상한. 행렬은 `image_matrix_for`(= `user_matrix_at` 기저 × w,h, 회전 쪽 포함). **마크가 없는 기존 이미지 id 는 `"#obj<쪽 객체 index>"`**, 첫 편집 때 `mark_object` 가 `onpdf-image-<16진>` 을 붙인다(Command 가 안정 id 로 바꿔 보관). **삭제·추가 취소는 문서가 떼어 낸 객체를 보관**(`detached_`, `DetachedId` — 같은 index 로 재삽입, `doc_` 뒤에 선언). Command: `AddImageCommand`(첫 실행 뒤 픽셀 해제, 취소=detach, 재실행=reattach, 밀려나면 `discard`→drop)·`MoveObjectsCommand`(텍스트 상자·이미지 공용; 화살표 연타는 `merge_with` 로 한 단계 — `UndoStack::push` 는 다시 실행이 없고 맨 위가 저장 지점이 아닐 때만 합친다)·`ResizeObjectCommand`·`RemoveObjectsCommand`. 텍스트 상자 폭은 마크 `width`(없으면 0 = 줄바꿈 없음), `layout_text(..., max_width)` 가 공백 기준 탐욕 줄바꿈(긴 단어는 글자 단위), `ReplaceTextBoxCommand(width, origin_x)` id `resize_text`. 뷰: 이미지 핸들 0~3(모서리), 텍스트 4/5(좌우) — 두 번 클릭은 핸들보다 먼저. 화살표는 `set_nudge_enabled`(권한만)로 받고 바쁜 동안 `MainWindow` 가 Δ 를 모았다가 실행. 저장 검증은 쪽별 마크 객체 id 집합 비교
- **폼 채우기(#51):** `PdfiumDocument` 가 AcroForm 이면 폼 필 환경을 소유한다(`detail::FormEnvironment`, `FPDF_FORMFILLINFO` 를 힙에 고정, **JS 플랫폼 없음**, 강조 alpha 0 — 강조는 앱이 그린다: 타일 캐시·인쇄에 섞이지 않게). 마지막 멤버라 가장 먼저 소멸하고, 이동 대입은 폼 환경 → 문서 → 바이트 순으로 직접 정리한다. 위젯은 **`FPDF_FFLDraw` 없이는 그려지지 않는다**: `render_page`·`render_region` 뒤 `draw_form`(타일은 `start = −region, size = 쪽 전체 px`, 위젯 없는 쪽은 건너뜀). 페이지 뷰는 `detail::FormPageView`(RAII, `FORM_OnAfterLoadPage/OnBeforeClosePage`)로 렌더·폼 작업 동안만. 쪽 삭제·이동·삽입 뒤 `reset_form()`(필드·페이지 뷰 재동기화). 위젯 정체성은 **`FieldRef` = 필드 전체 이름 + 그 쪽에서 같은 이름 위젯 가운데 순서(`nth`)** — PDFium 컨트롤 index 는 쓰지 않는다(쪽 삭제를 취소하면 되살린 위젯이 필드에 다시 붙어 index 가 밀린다), `/Annots` 전체 순서도 쓰지 않는다. 값 설정(공개 setter 없음): 텍스트 = `SetFocusedAnnot` → `SelectAllText` → `ReplaceSelection` → `ForceToKillFocus`, **편집 가능 콤보는 ReplaceSelection 을 무시**해 Backspace + `FORM_OnChar`, 체크·라디오 = 위젯 가운데 `FORM_OnLButtonDown/Up`(사용자 좌표, 상태가 다를 때만), 선택지 = `FORM_SetIndexSelected`. **라디오 그룹 해제는 공개 API 로 불가**(클릭·스페이스·NoToggleToOff 해제+환경 재생성 모두 실패) → 엔진 `kUnsupported`, 처음 선택 없던 그룹의 첫 선택 Command 의 undo 는 **문서를 바꾸지 않고 성공**(실패로 두면 스택이 막힌다), 앱이 안내. 여러 줄 값의 CRLF·CR 은 읽을 때 LF 로 정규화. 같은 이름 위젯은 값을 공유하므로 결과는 바뀐 쪽 전부(`pages_of_field`, 첫 사용 때 전체 쪽을 훑는 지도). 한글 값의 AP 는 PDFium 이 **비임베딩 Adobe-Korea1 CID 글꼴**(`KSCms-UHC-H`)로 만든다(Acrobat 과 같은 방식, 임베딩 AP 는 후속) — 테스트는 값·글꼴로 판정하고 한글 픽셀은 보지 않는다(시스템 글꼴 끔). 저장 전 `FORM_ForceToKillFocus`, 저장 검증은 필드 값 지도 비교. 앱: 위젯 스냅샷은 보이는 쪽 ±1 `load_form_widgets`(kLow), hit-test 순서 주석 → **위젯** → 쪽 객체. 텍스트 칸은 **Qt 오버레이 `FormFieldEditor`**(IME 는 Qt, Enter·Cmd+Enter(여러 줄)·Tab·포커스 잃음 = 확정, Esc = 취소, 값이 같으면 Command 없음, NFC 정규화), 체크·라디오는 클릭 즉시, 콤보·목록은 `QMenu`(`choose_form_option`, 편집 가능 콤보는 "직접 입력…"). Tab 은 텍스트 칸만 주석 순서로(읽기 전용 건너뜀, 쪽 끝이면 이웃 쪽). 폼 Command 는 **대기열**(`pending_form_commands_`)로 하나씩 — Tab 연타가 앞 편집보다 빠르다. 저장·닫기·인쇄 전 `settle_form_edits()`(편집기 확정 + 대기열이 빌 때까지 이벤트 루프, 상한 5초). 게이트는 `permissions.fill_forms`(비트 6 또는 R≥3 비트 9), 읽기 전용·푸시 버튼·서명 칸은 안내만. XFA 전체 문서는 AcroForm 부분만 표시하고 안내
- **병합·분할(#37):** 현재 문서를 바꾸지 않는 내보내기라 Command 대상이 아니다. `QpdfTools` 작업은 PDFium 워커가 아닌 `StructureJob`(전용 `std::jthread`, 한 번에 하나)에서 돌고 결과는 `UiDispatcher::post_to` 로. 출력은 `write_files_safely`(임시 → qpdf `check` + 쪽 수 → 교체, 분할은 전부-또는-없음). 병합은 첫 파일이 주 입력(그 목차·폼만 보존 — qpdf 는 다른 입력의 목차를 버린다), 모든 입력은 추출 권한(비트 5) 필요, 결과는 평문. 작업 JSON 에 `decrypt` 를 넣어 **결과는 항상 평문**(주 입력이 암호 문서여도 — 그렇지 않으면 검증이 열지 못한다). 암호 오류 판정은 `qpdf_get_error_code == qpdf_e_password` 또는 정확한 문구만(경로에 "password" 가 든 파일을 오판하지 않게). qpdf 작업은 중단 API 가 없어 취소 = 결과를 열지 않기만(파일은 **지우지 않는다** — 사용자가 덮어쓰기로 고른 파일일 수 있다). 분할은 디스크 파일을 세션이 보관한 암호로 읽으므로 수정된 문서는 먼저 `maybe_save`, 기존 파일이 있으면 덮어쓰기 확인. vcpkg 의 qpdf 설정이 zlib·jpeg 를 맨이름으로 적어 `core/CMakeLists.txt` 가 `ZLIB::ZLIB`·`JPEG::JPEG` 로 바꾼다
- **썸네일:** 다중 선택(`ExtendedSelection`), 현재 쪽 강조는 선택과 분리. 끌어 놓기는 모델이 행을 옮기지 않고 `move_requested(pages, drop_row)` 만 내며(`dropMimeData` 는 항상 false), `MainWindow::move_pages` 가 `dest = drop_row − |{p < drop_row}|` 로 바꿔 Command 를 실행한다. 파일 URL 드롭은 삽입. Delete 키·우클릭 메뉴. 편집 불가 상태면 `set_editable(false)` 로 끌기 자체를 막는다. 삽입 소스 암호는 `failed(kPasswordRequired)` 뒤 `insert_password` 대화상자로 다시 시도, 추출 금지(권한 비트 5)는 `kPermissionDenied`. 서명 문서의 첫 구조 편집은 `signature_warning` 으로 한 번 묻는다

### 렌더링 (구현: core/view + app/DocumentView)
- 쪽을 512×512 타일로 나눠 `PdfiumDocument::render_region`(`FPDF_RenderPageBitmapWithMatrix`, 회전 0 기준 — `page_size` 가 /Rotate 반영) 으로 그리고 `TileCache`(기본 256MB LRU) 에 보관
- `TileScheduler::update(viewport)` 가 보이는 타일·쪽 미리보기(배율 1/8, 4,096pt 초과 쪽은 `preview_scale_q_for` 로 낮춤)를 kHigh, 앞뒤 1쪽(타일 ≤32개일 때)을 kLow 로 요청하고 밀려난 요청은 `TaskHandle::cancel`
- 뷰는 쪽 배경 → 미리보기 → 이전 배율 타일(늘려 그림) → 현재 배율 타일 순으로 그려 확대·스크롤 중 빈 화면이 없다
- 링크·목차 대상: 링크는 텍스트 추출 때 대상 위치를 쪽 포인트(`Link::location`)로 바로 변환한다. 목차는 항목이 많아 로드 때 쪽을 열지 않고 `user_location`(사용자 좌표)만 두며, 이동할 때 `MainWindow::navigate_to` 가 `resolve_location`(kHigh) 으로 변환
- 현재 쪽: `go_to_page`·`scroll_to_page_point` 직후 스크롤 값이 그대로인 동안은 이동한 쪽(이동 고정 쪽), 그 밖에는 뷰포트 중앙 쪽. 짧은 쪽·문서 끝에서도 쪽 번호가 유지된다
- 텍스트 인덱스가 없는 쪽의 드래그 지점은 보류했다가 인덱스 도착 시 다시 계산한다. 선택 변경은 선택 사각형 영역만 다시 그린다
- 배율은 `quantize_scale(zoom × dpr)` 로 1/64 단위. 맞춤 배율은 내림(`fit_*_scale_q`) 이라 문서가 뷰포트를 넘지 않는다
- 타일 경계는 AGG 클리핑 때문에 전체 렌더와 픽셀 단위로 완전히 같지는 않다(0.03% 수준). `tile_stitch_test` 가 0.1% 상한으로 검증
- 사이드바(F4): `ThumbnailPanel` 은 보이는 행 ±4 쪽만 `render_page`(kLow, 동시 8) → `QCache` 64MB, 문서 교체·폭 32px 이상 변화·화면 배율 변경(`DevicePixelRatioChange`) 시 세대 증가로 늦은 결과를 버린다. 스크롤바를 끄는 동안은 요청하지 않는다. 목차 강조·상태 표시줄 통계는 스로틀(100ms·1초)로 갱신. `OutlinePanel` 은 `load_outline`(kNormal) 스냅샷을 표시하고, /XYZ 이동은 `resolve_location`(kHigh) → `scroll_to_page_point`. 현재 항목은 `DocumentView::reading_position` + `Outline::item_at`
- 인쇄(Ctrl/Cmd+P): 래스터 인쇄. `PrintJob` 이 쪽을 `render_page`(kNormal, `printing=true`, 렌더 중+대기 비트맵 ≤2장)로 그려 메인 스레드에서 쪽 순서대로 `QPrinter` 에 `drawImage`. 권한(비트 3·12)·`/PrintScaling`·`/NumCopies`·`/PrintPageRange`(1 기준)·`/Duplex` 반영. 진행 창은 비모달(모달 `QProgressDialog::setValue` 는 이벤트를 재진입 처리). `finished` 는 다음 이벤트 턴에 보낸다. 인쇄 중 다른 문서를 열면 먼저 묻는다("인쇄 취소하고 열기" / "인쇄 계속하기"), 계속하기면 열지 않는다. 앱 테스트는 `QPrinter::PdfFormat` 출력을 다시 열어 검증

# Coding Guidelines
1. **Safety First**: 모든 PDFium 반환값(nullptr, FALSE, 음수)을 검사한다. 파일 I/O 와 외부 라이브러리 호출은 `std::expected` 또는 명시적 에러 타입으로 전파하고, 예외는 경계(UI·CLI)에서만 잡는다.
2. **Ownership 명확히**: raw `new`/`delete` 금지. 소유권은 `unique_ptr`, 공유는 꼭 필요할 때만 `shared_ptr`, 비소유 참조는 참조 또는 raw pointer(nullable 명시).
3. **스레드**: PDFium 워커 외 스레드에서 PDFium 객체 접근 금지. 공유 상태는 `std::mutex` 또는 메시지 전달로 보호. 데이터 레이스는 Critical 이슈로 취급.
4. **텍스트는 UTF-8**: 유니코드 매핑 오류 글자와 유니코드 스칼라가 아닌 값은 U+FFFD(`view::sanitize_text_code`). 내부 문자열은 UTF-8 `std::string`. PDFium 의 UTF-16LE 와는 core/engine 경계에서만 변환. 한글(CID 폰트) 경로는 항상 테스트를 함께 만든다. 글자 좌표는 PDFium 사용자 좌표(좌하단, 회전 전)가 아니라 `TextPage` 가 `FPDF_PageToDevice`(×1000 트릭)로 바꾼 "쪽 포인트"(좌상단, 회전 반영)를 쓴다.
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
