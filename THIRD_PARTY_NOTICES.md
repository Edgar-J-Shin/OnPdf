# 제3자 고지

OnPdf 는 Apache License 2.0 으로 배포되며, 아래 제3자 구성 요소를 사용한다. 각 구성 요소는 해당 라이선스를 따른다.

## 배포물에 포함되는 구성 요소

### PDFium
- 출처: https://pdfium.googlesource.com/pdfium (chromium/8152), 사전 빌드 https://github.com/bblanchon/pdfium-binaries
- 라이선스: BSD-3-Clause (PDFium), MIT (pdfium-binaries 빌드 스크립트)
- PDFium 공유 라이브러리에는 다음 구성 요소가 정적으로 들어 있다. 전체 라이선스 문구는 사전 빌드의 `licenses/` 폴더에 있다.

| 구성 요소 | 라이선스 |
|---|---|
| Abseil | Apache-2.0 |
| Anti-Grain Geometry 2.3 | AGG 라이선스(BSD 계열, 사용·복제·수정·배포 허용) |
| Dragonbox | Apache-2.0 WITH LLVM-exception 또는 BSL-1.0 |
| fast_float | MIT |
| FreeType | FreeType License (FTL) |
| HarfBuzz | Old MIT |
| Highway | Apache-2.0 |
| ICU | Unicode License v3 |
| Little CMS (lcms2) | MIT |
| libjpeg-turbo | IJG License 및 BSD-3-Clause |
| OpenJPEG | BSD-2-Clause |
| libpng | PNG Reference Library License v2 |
| LLVM libc | Apache-2.0 WITH LLVM-exception |
| simdutf | MIT |
| zlib | zlib License |

### Qt 6
- 출처: https://www.qt.io (Qt Core·Gui·Widgets·PrintSupport)
- 라이선스: GNU LGPL v3
- OnPdf 는 Qt 를 **동적으로 링크**한다. 사용자는 호환되는 다른 Qt 라이브러리로 교체해 실행할 수 있다. Qt 소스는 https://download.qt.io 에서 받을 수 있다.

### stb (stb_image, stb_image_write)
- 출처: https://github.com/nothings/stb
- 라이선스: MIT 또는 퍼블릭 도메인(Unlicense) 중 선택

### qpdf
- 출처: https://qpdf.sourceforge.io (vcpkg 포트 12.4.2, 라이브러리만 — 내장 암호 공급자)
- 라이선스: Apache-2.0 (일부 파일 MIT). 전체 문구는 qpdf 배포본의 `LICENSE.txt`·`NOTICE.md`
- 용도: PDF 병합·분할·구조 검사. macOS·Linux 는 정적 링크, Windows 는 `qpdf*.dll` 로 함께 배포

### zlib (qpdf 의존)
- 출처: https://zlib.net (vcpkg)
- 라이선스: zlib License

### libjpeg-turbo (qpdf 의존)
- 출처: https://libjpeg-turbo.org (vcpkg)
- 라이선스: IJG License 및 BSD-3-Clause (SIMD 코드 일부는 zlib License)

### HarfBuzz
- 출처: https://github.com/harfbuzz/harfbuzz (vcpkg 포트 15.1.0, `freetype` feature 끔)
- 라이선스: MIT ("Old MIT"). 전체 문구는 배포본의 `COPYING`
- 용도: 새 텍스트의 글자 배치·측정과 서브셋 임베딩(hb-subset). macOS·Linux 는 정적 링크(edit 계층), Windows 는 vcpkg 기본 triplet 이라 `harfbuzz*.dll` 로 함께 배포

### Noto Sans KR (번들 글꼴)
- 출처: https://github.com/google/fonts/tree/main/ofl/notosanskr (`third_party/fonts-version.txt` 의 커밋으로 고정, `third_party/fetch_fonts.py`)
- 라이선스: SIL Open Font License 1.1 (`fonts/OFL-NotoSansKR.txt` 를 글꼴과 함께 배포). 이름을 바꾸지 않고 원본 그대로 번들하며, 문서에는 서브셋을 임베딩한다(OFL 허용)
- 용도: 텍스트 추가 도구의 기본 글꼴

## 개발·테스트에만 쓰는 구성 요소 (배포물에 포함되지 않음)

| 구성 요소 | 용도 | 라이선스 |
|---|---|---|
| GoogleTest | 단위·앱 테스트 | BSD-3-Clause |
| qpdf (명령줄) | 테스트 PDF 생성(AES-256 암호화 샘플) | Apache-2.0 |

## 계획된 구성 요소
- FreeType·HarfBuzz 직접 사용(글자 배치, M3)은 도입할 때 이 문서에 추가한다.
