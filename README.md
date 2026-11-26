# OnPdf

PDFium 기반의 가볍고 빠른 Windows·macOS용 오픈소스 PDF 뷰어·편집기입니다.

> 현재 단계: **v0.5.3** — [Releases](https://github.com/Edgar-J-Shin/OnPdf/releases) 에서 Windows 설치 파일·무설치 zip 과 macOS DMG(Apple Silicon·Intel, macOS 13 이상)를 받을 수 있습니다. v1.0 전에 코드 서명·공증이 남았습니다. 변경 내역은 [CHANGELOG.md](CHANGELOG.md) 를 참고하세요.

## 목표

- 100쪽 문서를 1초 안에 첫 화면 표시, 스크롤 중 끊김 없음
- 열기·저장 왕복 후 깨지는 파일 0건
- 저장한 파일이 Adobe Reader, Chrome, Edge 에서 동일하게 보임
- 한글 문서에서 텍스트 추가·수정 시 글자 깨짐 없음

## 로드맵

| 단계 | 내용 | 버전 |
|---|---|---|
| M0 | 저장소, CMake, PDFium 연결, 첫 쪽 표시 | 완료 |
| M1 | 뷰어: 연속 스크롤, 확대·축소, 검색, 썸네일, 목차, 인쇄 | v0.1 (완료) |
| M2 | 주석, 페이지 조작(회전·삭제·재배열·병합·분할), 저장, 실행 취소 | v0.2 (완료) |
| M3 | 텍스트·이미지 추가, 객체 편집, 폼 채우기, 폰트 임베딩 | v0.3 (완료) |
| M4 | 기존 텍스트 줄 단위 수정, 서식 변경, 시스템 글꼴 대체, 폭 맞춤 | v0.5 (완료) |
| M5 | 배포물 자동 생성, 다국어, 인쇄 미리보기, 자간 유지, Windows 설치기, macOS x64·13 (v0.5.3 완료) · 코드 서명·공증 | v1.0 |

자세한 계획은 [pdf-editor-plan.md](pdf-editor-plan.md) 를 참고하세요.

## 기술 스택

- C++23, CMake + vcpkg, Ninja
- [PDFium](https://pdfium.googlesource.com/pdfium/) ([pdfium-binaries](https://github.com/bblanchon/pdfium-binaries) 사전 빌드)
- [qpdf](https://github.com/qpdf/qpdf), Qt 6 Widgets (동적 링크), HarfBuzz + FreeType
- GoogleTest, GitHub Actions

## 설치

[Releases](https://github.com/Edgar-J-Shin/OnPdf/releases) 에서 내려받습니다.

- **Windows x64**: `OnPdf-<버전>-windows-x64-setup.exe` 를 실행합니다(권장 — 관리자 권한 없이 사용자 폴더에 설치, 시작 메뉴·"연결 프로그램" 등록, 앱 및 기능에서 제거). 설치 없이 쓰려면 `OnPdf-<버전>-windows-x64.zip` 을 풀고 `onpdf_app.exe` 를 실행합니다. 코드 서명 인증서가 아직 없어 SmartScreen 경고가 뜨면 "추가 정보 → 실행" 을 누르세요. "PDF 파일을 OnPdf 로 열기" 를 켜도 Windows 가 기본 앱 확인을 요청할 수 있습니다(설정 → 앱 → 기본 앱).
- **macOS 13 이상**: Apple Silicon 은 `OnPdf-<버전>-macos-arm64.dmg`, Intel Mac 은 `OnPdf-<버전>-macos-x64.dmg` 를 열어 `OnPdf.app` 을 응용 프로그램 폴더로 끌어 놓습니다. 공증 전 빌드는 처음 한 번 Control-클릭 → "열기" 가 필요합니다.
- 내려받은 파일은 같은 릴리스의 `SHA256SUMS.txt` 로 확인할 수 있습니다.

## 빌드

```bash
# 사전 준비: cmake ≥ 3.25, ninja, vcpkg (full clone) + export VCPKG_ROOT=<vcpkg 경로>
python3 third_party/fetch_pdfium.py          # PDFium 바이너리 다운로드 (third_party/pdfium/<platform>/)
python3 third_party/fetch_fonts.py           # 번들 글꼴 Noto Sans KR 다운로드 (third_party/fonts/, 텍스트 추가 도구)
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/bin/onpdf_cli render some.pdf page0.png

# Qt 앱 (Qt 6.7 이상 + Linguist 도구 필요. macOS: brew install qt && export QT_ROOT=$(brew --prefix qt))
cmake --preset dev-app && cmake --build --preset dev-app
./build/dev-app/bin/onpdf_app some.pdf
```

## 저장소 구조

```
core/         문서 모델, 렌더 캐시, PDFium·qpdf 래퍼 (Qt 의존 없음)
edit/         명령(Command), 실행 취소, 텍스트 블록 모델, 폰트 매니저
app/          Qt UI
cli/          렌더·병합·검증용 명령줄 도구
tests/        단위 테스트, 렌더링 회귀 테스트
testdata/     테스트 PDF (라이선스가 명확한 파일만)
third_party/  PDFium 바이너리 다운로드 스크립트
docs/         설계 문서, 기여 가이드
```

## 기여

git flow 로 운영합니다. `develop` 이 기본 브랜치이고 `main` 은 릴리스 전용입니다.
작업은 이슈를 먼저 만든 뒤 `feature/<이슈번호>-<설명>` 브랜치에서 진행하고, `develop` 으로 PR 을 보냅니다.
PR 은 CI(Windows·macOS·Linux) 통과 후 머지합니다.

## 라이선스

[Apache License 2.0](LICENSE). PDFium, qpdf, FreeType, HarfBuzz 는 각자의 허용적 라이선스를, Qt 는 LGPL v3 조건(동적 링크)을 따릅니다.
