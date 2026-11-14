# testdata

테스트에 쓰는 PDF 와 기준 이미지. **라이선스가 명확한 파일만** 넣는다.
파일별 기대값(열기 결과·암호·쪽 수·첫 쪽 크기)은 `manifest.tsv` 가 단일 출처이며, `onpdf_unit_tests` 의 manifest 테스트가 전 파일을 검증한다. 아래 표는 `gen/make_all.py` 가 manifest 에서 생성한다.

<!-- manifest:start -->
| 파일 | 열기 | 암호 | 쪽 | 출처 | 라이선스 | 용도 |
|---|---|---|---|---|---|---|
| `simple.pdf` | ok |  | 2 | gen | Apache-2.0 | 기본: 2쪽(세로/가로), Helvetica, 도형 |
| `korean-embedded.pdf` | ok |  | 1 | gen | Apache-2.0 | 한글 CIDFontType2 임베딩(Identity-H), M3 폰트 임베딩 기준 |
| `korean-system-font.pdf` | ok |  | 1 | gen | Apache-2.0 | 비임베딩 한글 CID(Adobe-Korea1, UniKS-UCS2-H) → 시스템 폰트 대체 |
| `encrypted-rc4-user.pdf` | password | user `user` / owner `owner` | 2 | gen | Apache-2.0 | 표준 보안 핸들러 R3 RC4-128, 사용자 암호 |
| `encrypted-owner-only.pdf` | ok | user `` / owner `owner` | 2 | gen | Apache-2.0 | 소유자 암호만(P=-3904), 암호 없이 열림 |
| `encrypted-aes256.pdf` | password | user `user` / owner `owner` | 2 | gen+qpdf | Apache-2.0 | AESV3 R6 256-bit (qpdf) |
| `scanned.pdf` | ok |  | 3 | gen | Apache-2.0 | 이미지 전용 3쪽(8-bit Gray, Flate), 텍스트 없음 |
| `form-acro.pdf` | ok |  | 1 | gen | Apache-2.0 | AcroForm: 텍스트·체크박스·라디오·콤보(외형 스트림 포함). 위젯은 폼 필 환경(FPDF_FFLDraw, M3) 없이는 그려지지 않아 현재 기준 PNG 에는 보이지 않음 |
| `bookmarks.pdf` | ok |  | 10 | gen | Apache-2.0 | 3단계 Outline(한글 제목), 10쪽 |
| `large-1000.pdf` | ok |  | 1000 | gen | Apache-2.0 | 1,000쪽, xref 스트림 + 객체 스트림 |
| `mixed-sizes.pdf` | ok |  | 7 | gen | Apache-2.0 | A4/Letter/A5/명함, /Rotate 90·270 (p5 렌더 비교) |
| `xref-stream.pdf` | ok |  | 2 | gen | Apache-2.0 | PDF 1.5 xref 스트림 + 객체 스트림 |
| `incremental-update.pdf` | ok |  | 3 | gen | Apache-2.0 | 증분 갱신으로 3쪽 추가 (/Prev) |
| `annotations.pdf` | ok |  | 2 | gen | Apache-2.0 | Highlight(AP, BM Multiply)·Square·Text(PDFium 생성 AP)·Link 3종(URI, 내부 GoTo, 차단 스킴) |
| `empty-page.pdf` | ok |  | 1 | gen | Apache-2.0 | 내용 없는 1쪽 |
| `unicode-metadata.pdf` | ok |  | 1 | gen | Apache-2.0 | Info 한글 제목·작성자(UTF-16BE) |
| `transparency.pdf` | ok |  | 1 | gen | Apache-2.0 | ExtGState ca/CA 0.5 겹친 원 |
| `truncated.pdf` | invalid |  | 0 | gen | Apache-2.0 | bookmarks.pdf 뒤 40% 절단(쪽 트리 소실) → 복구 불가, 크래시 없이 invalid |
| `bad-xref.pdf` | ok |  | 2 | gen | Apache-2.0 | xref 오프셋 전부 오류 → 복구 파서 |
| `garbage-header.pdf` | ok |  | 2 | gen | Apache-2.0 | %PDF 앞 1KB 쓰레기 → 헤더 탐색 |
| `한글 파일명.pdf` | ok |  | 1 | gen | Apache-2.0 | unicode-metadata 복사본, 경로 UTF-8 처리 |
| `pdfium-hello_world.pdf` | ok |  | 1 | pdfium | BSD-3-Clause | 최소 텍스트 문서 |
| `pdfium-encrypted.pdf` | password | user `1234` / owner `5678` | 1 | pdfium | BSD-3-Clause | PDFium 암호 샘플 |
| `pdfium-bookmarks.pdf` | ok |  | 2 | pdfium | BSD-3-Clause | 북마크 샘플 |
| `pdfium-text_form.pdf` | ok |  | 1 | pdfium | BSD-3-Clause | AcroForm 텍스트 필드 |
| `pdfium-rectangles.pdf` | ok |  | 1 | pdfium | BSD-3-Clause | 도형 렌더 샘플 |
| `pdfium-trailer_unterminated.pdf` | invalid |  | 0 | pdfium | BSD-3-Clause | 손상: trailer 미종료 |
| `pdfium-bug_481363.pdf` | ok |  | 1 | pdfium | BSD-3-Clause | 실제 버그 리포트 손상 파일 |
| `not_a_pdf.txt` | invalid |  | 0 | gen | Apache-2.0 | PDF 가 아닌 파일 |
<!-- manifest:end -->

## 출처·라이선스
- `gen` = 이 저장소의 `gen/docs.py` 로 생성 (Apache-2.0, 프로젝트와 동일). 날짜·난수를 쓰지 않아 재실행해도 바이트가 같다
- `gen+qpdf` = `simple.pdf` 를 qpdf 로 AES-256 암호화 (출력에 임의 IV 가 들어가 결정적이지 않으므로 `--check` 대상에서 제외)
- `pdfium` = PDFium `testing/resources` (chromium/8152), BSD-3-Clause — `licenses/PDFium-LICENSE.txt`
- `fonts/NotoSansKR-subset.ttf` = Noto Sans KR 의 wght 400 서브셋(family 이름 변경), SIL OFL 1.1 — `licenses/OFL.txt`

## 갱신 절차
```bash
python3 testdata/gen/make_all.py            # 생성 + README 표 갱신 (qpdf 있으면 AES 파일도)
python3 testdata/gen/make_all.py --check    # CI: 커밋본과 바이트 비교
python3 testdata/gen/fetch_pdfium_samples.py   # PDFium 샘플 다시 받기 (버전 올릴 때)
pip install fonttools brotli && python3 testdata/gen/fetch_font.py   # 한글 서브셋 폰트 갱신 (글자 추가 시)
ONPDF_UPDATE_REFERENCES=1 ./build/dev/bin/onpdf_render_tests         # 렌더링 기준 PNG 갱신
```
- 새 생성 문서: `gen/docs.py` 에 `make_*` 추가 → `make_all.py` 의 `GENERATED` 에 등록 → `manifest.tsv` 에 행 추가 → 생성·커밋
- 한글 임베딩 문서에 새 글자를 쓰려면 `fetch_font.py` 의 `TEXT` 에 넣고 서브셋을 다시 만든다
- 손상 파일의 `expect` 는 PDFium 복구 결과를 실측해 적는다 (`onpdf_cli info`)
- `--check` 의 바이트 비교는 Flate 출력이 zlib 구현에 의존한다 (CPython 기본 zlib 기준). zlib-ng 를 쓰는 Python 에서 실패하면 환경 문제이지 데이터 변경이 아니다
