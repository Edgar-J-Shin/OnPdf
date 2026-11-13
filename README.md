# OnPdf

PDFium 기반의 가볍고 빠른 Windows용 오픈소스 PDF 뷰어·편집기입니다.

> 현재 단계: **M0 기반** — 저장소·빌드·PDFium 연결 작업 중입니다. 아직 사용할 수 있는 빌드가 없습니다.

## 목표

- 100쪽 문서를 1초 안에 첫 화면 표시, 스크롤 중 끊김 없음
- 열기·저장 왕복 후 깨지는 파일 0건
- 저장한 파일이 Adobe Reader, Chrome, Edge 에서 동일하게 보임
- 한글 문서에서 텍스트 추가·수정 시 글자 깨짐 없음

## 로드맵

| 단계 | 내용 | 버전 |
|---|---|---|
| M0 | 저장소, CMake, PDFium 연결, 첫 쪽 표시 | — |
| M1 | 뷰어: 연속 스크롤, 확대·축소, 검색, 썸네일, 목차, 인쇄 | v0.1 |
| M2 | 주석, 페이지 조작(회전·삭제·재배열·병합·분할), 저장, 실행 취소 | v0.2 |
| M3 | 텍스트·이미지 추가, 객체 편집, 폼 채우기, 폰트 임베딩 | v0.3 |
| M4 | 기존 텍스트 줄 단위 수정 | v0.5 |
| M5 | 성능, 설치 파일, 다국어, 문서화 | v1.0 |

자세한 계획은 [pdf-editor-plan.md](pdf-editor-plan.md) 를 참고하세요.

## 기술 스택

- C++20, CMake + vcpkg, Ninja
- [PDFium](https://pdfium.googlesource.com/pdfium/) ([pdfium-binaries](https://github.com/bblanchon/pdfium-binaries) 사전 빌드)
- [qpdf](https://github.com/qpdf/qpdf), Qt 6 Widgets (동적 링크), HarfBuzz + FreeType
- GoogleTest, GitHub Actions

## 빌드

```bash
# 사전 준비: cmake, ninja, vcpkg (VCPKG_ROOT 설정)
python3 third_party/fetch_pdfium.py          # PDFium 바이너리 다운로드
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
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

## 라이선스

[Apache License 2.0](LICENSE). PDFium, qpdf, FreeType, HarfBuzz 는 각자의 허용적 라이선스를, Qt 는 LGPL v3 조건(동적 링크)을 따릅니다.
