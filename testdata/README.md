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
| `perm-modify-only.pdf` | ok | user `` / owner `owner` | 2 | gen | Apache-2.0 | 내용 수정(비트 4)만 허용(P=-3896) — 텍스트 도구 켜짐, 주석 꺼짐 (#45) |
| `encrypted-aes256.pdf` | password | user `user` / owner `owner` | 2 | gen+qpdf | Apache-2.0 | AESV3 R6 256-bit (qpdf) |
| `scanned.pdf` | ok |  | 3 | gen | Apache-2.0 | 이미지 전용 3쪽(8-bit Gray, Flate), 텍스트 없음 |
| `form-acro.pdf` | ok |  | 1 | gen | Apache-2.0 | AcroForm: 텍스트·체크박스·라디오·콤보(외형 스트림 포함). 위젯은 폼 필 환경의 FPDF_FFLDraw 로 그린다 (#51) |
| `form-fields.pdf` | ok |  | 2 | gen | Apache-2.0 | 폼 채우기(#51): 한 줄·여러 줄·읽기 전용·MaxLen 텍스트, 체크박스, 기본 선택 없는 라디오, 편집 가능 콤보, 다중 목록. 2쪽(/Rotate 90)에 이름 필드 둘째 위젯·서명 칸 |
| `bookmarks.pdf` | ok |  | 10 | gen | Apache-2.0 | 3단계 Outline(한글 제목), 10쪽 |
| `large-1000.pdf` | ok |  | 1000 | gen | Apache-2.0 | 1,000쪽, xref 스트림 + 객체 스트림 |
| `mixed-sizes.pdf` | ok |  | 7 | gen | Apache-2.0 | A4/Letter/A5/명함, /Rotate 90·270 (p5 렌더 비교) |
| `xref-stream.pdf` | ok |  | 2 | gen | Apache-2.0 | PDF 1.5 xref 스트림 + 객체 스트림 |
| `incremental-update.pdf` | ok |  | 3 | gen | Apache-2.0 | 증분 갱신으로 3쪽 추가 (/Prev) |
| `annotations.pdf` | ok |  | 2 | gen | Apache-2.0 | Highlight(AP, BM Multiply)·Square·Text(PDFium 생성 AP)·Link 3종(URI, 내부 GoTo, 차단 스킴) |
| `annotation-popup.pdf` | ok |  | 1 | gen | Apache-2.0 | Text 메모 + 연결된 /Popup — 메모 삭제 시 Popup 도 함께 지우는지 (I8) |
| `empty-page.pdf` | ok |  | 1 | gen | Apache-2.0 | 내용 없는 1쪽 |
| `unicode-metadata.pdf` | ok |  | 1 | gen | Apache-2.0 | Info 한글 제목·작성자(UTF-16BE) |
| `transparency.pdf` | ok |  | 1 | gen | Apache-2.0 | ExtGState ca/CA 0.5 겹친 원 |
| `text-layout.pdf` | ok |  | 1 | gen | Apache-2.0 | 줄 묶음 휴리스틱: 회전 Tm, 1 Tf+Tm 스케일, 위첨자, 2단 |
| `outline-cycle.pdf` | ok |  | 2 | gen | Apache-2.0 | 악성 목차: 형제 순환·자식 자기 참조·깊이 40 체인 → 유한 항목, 크래시 없음 |
| `print-flags.pdf` | ok |  | 2 | gen | Apache-2.0 | 인쇄 플래그 주석(화면 전용 /F 0·인쇄 전용 /F 36) + ViewerPreferences(PrintScaling None·NumCopies 2·PrintPageRange 2쪽·양면) |
| `print-low-res.pdf` | ok |  | 2 | gen | Apache-2.0 | 소유자 암호만(P=-2052): 인쇄 허용, 고품질 인쇄 금지 → 저해상도만 |
| `unicode-map-error.pdf` | ok |  | 1 | gen | Apache-2.0 | ToUnicode 없는 Type3 글꼴(AGL 밖 글리프 이름) → PDFium 매핑 오류, U+FFFD 로 추출 |
| `signed-dummy.pdf` | ok |  | 1 | gen | Apache-2.0 | 전자서명 필드 1개(가짜 서명값) — 서명 수 1, 저장은 증분 저장으로 원본 바이트 보존 |
| `perm-assemble-only.pdf` | ok |  | 2 | gen | Apache-2.0 | 소유자 암호만(R3, P=-12): 내용 수정 금지·쪽 조립 허용 → 쪽 회전 가능 |
| `perm-no-assemble.pdf` | ok |  | 2 | gen | Apache-2.0 | 소유자 암호만(R3, P=-1028): 쪽 조립(비트 11)만 금지 → 쪽 회전 불가 |
| `perm-no-extract.pdf` | ok |  | 2 | gen | Apache-2.0 | 소유자 암호만(R3, P=-17): 복사·추출(비트 5)만 금지 → 쪽 삽입 소스로 쓸 수 없다 |
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
| `text-lines-basic.pdf` | ok |  | 1 | gen | Apache-2.0 | 기존 줄 수정(#65): 평범한 줄, Tc/Tw 줄, 굵게 섞인 줄, 같은 BT 의 두 Tj(밀림), TJ·'·" 연산자, 지울 줄, 도형 |
| `text-lines-basic-enc.pdf` | password | user `user` / owner `owner` | 1 | gen | Apache-2.0 | text-lines-basic 의 RC4 암호본 — 암호 유지 수술 |
| `text-lines-form.pdf` | ok |  | 4 | gen | Apache-2.0 | Form XObject 1단계 줄(수정 가능)·2단계 줄(불가), 쪽 직속 줄, 이미지; 2쪽은 BT 안에서 나뉜 /Contents 배열(밀림); 3·4쪽은 공유 Form(불가) |
| `text-lines-type3.pdf` | ok |  | 1 | gen | Apache-2.0 | Type3 줄(대체 글꼴)·임베딩 CID 한글 줄(원본 글꼴 재사용)·보이지 않는 3 Tr 줄(불가) |
| `text-lines-kern.pdf` | ok |  | 1 | gen | Apache-2.0 | 줄 폭 맞춤(#77): TJ 넓힘·Tc 자간·끝 공백·좁힘 커닝·보통·큰 자간(한계)·배열 문자열 하나 TJ 줄 |
<!-- manifest:end -->

## 출처·라이선스
- `gen` = 이 저장소의 `gen/docs.py` 로 생성 (Apache-2.0, 프로젝트와 동일). 날짜·난수를 쓰지 않아 재실행해도 바이트가 같다
- `gen+qpdf` = `simple.pdf` 를 qpdf 로 AES-256 암호화 (출력에 임의 IV 가 들어가 결정적이지 않으므로 `--check` 대상에서 제외)
- `pdfium` = PDFium `testing/resources` (chromium/8152), BSD-3-Clause — `licenses/PDFium-LICENSE.txt`
- `fonts/NotoSansKR-subset.ttf` = Noto Sans KR 의 wght 400 서브셋(family 이름 변경), SIL OFL 1.1 — `licenses/OFL.txt`
- `fonts/NotoSansCJKkr-subset.otf` = Noto Sans CJK KR(OTF, CID-keyed CFF)의 같은 글자 서브셋 — CFF 저장 후처리·charset 테스트(#73), SIL OFL 1.1
- `fonts/NotoSansKR-subset.ttc` = 위 TTF 서브셋 2벌(면 1 family `NotoSansKR-subset-Face1`) — TTC 면 선택 테스트(#73)
- `fonts/SourceSans3-subset.otf` = Adobe Source Sans 3(OTF, 비CID CFF)의 라틴 서브셋 — charset 을 손대지 않는 변환 경로 테스트(#73 리뷰 W4), SIL OFL 1.1

## 왕복 코퍼스 (`corpus.tsv`, #119)
계획서 성공 기준 "테스트 PDF 200개 이상에서 열기·저장 왕복 후 깨지는 파일 0건" 을 위한 공개 PDF 모음. **파일은 커밋하지 않고**
목록만 커밋한다 — `python3 testdata/gen/fetch_corpus.py` 가 `corpus/<source>/<name>` 으로 받는다(gitignore, 약 10 MB).
`onpdf_corpus_tests` 가 파일마다 열기 → 안전 저장 → 다시 열기 비교(쪽 수·크기·첫 쪽 텍스트·렌더) → qpdf 검사를 돌고,
파일이 없으면 SKIP(CI 는 `ONPDF_CORPUS_REQUIRED=1` 로 실패 처리).

| 출처 | 라이선스 근거 | 선정 |
|---|---|---|
| `pdfium` — PDFium `testing/resources` | BSD-3-Clause(Google 작성 테스트 파일, `licenses/PDFium-LICENSE.txt`). 커밋 = `third_party/pdfium-version.txt` 와 같은 브랜치 | 70 |
| `qpdf` — qpdf `qpdf/qtest/qpdf` | Apache-2.0(저자 작성 테스트 파일, `licenses/qpdf-LICENSE.txt`) | 80 |
| `pdfjs` — pdf.js `test/pdfs` | Apache-2.0(`licenses/pdf.js-LICENSE.txt`). 저장소의 `test/pdfs/.gitignore` 가 `*.pdf` 를 막고 **재배포 가능한 파일만** `!` 로 허용하므로 저장소 안 파일만 쓴다(`.link` 파일 제외) | 80 |
| `pdfbox` — Apache PDFBox `pdfbox/src/test/resources` | Apache-2.0(ASF 기여 규정, `licenses/PDFBox-LICENSE.txt`·`PDFBox-NOTICE.txt`) | 30 |

저장소·커밋 SHA·라이선스는 `gen/fetch_corpus.py` 의 `SOURCES` 한 곳에 있고 파일마다 sha256 을 둔다(상류가 바뀌어도 같은 바이트).
pdfcpu 테스트 자료는 제3자 저작물(ITU-T T.81·Adobe 정오표 등)이 섞여 있어 쓰지 않는다.

**선정 규칙**(2026-11-27, 후보 2,206개 → 260개): 파일당 ≤ 3 MB, 출처별 상한 70/80/80/30, sha256 중복 제거, 암호를 아는 암호 파일 전부(30),
열리지 않는 파일(`invalid`) 8, 특성 태그(`note` 열 — 암호 3종·손상·폼·XFA·주석·한글/CJK·세로쓰기·JBIG2/JPX/CCITT·Type3/CFF/CID·xref/객체 스트림·
선형화·증분·PDF 2.0·투명도·셰이딩·패턴·서명·JavaScript·100쪽 이상)마다 3개 이상, 나머지는 해시 순으로 보충. 100쪽 이상은 공개 코퍼스에
하나뿐이라(pdf.js `freeculture.pdf` 352쪽) 다쪽 검증은 저장소의 `large-1000.pdf` 가 맡는다.
- **넣지 않는 것**: 문서는 열리지만 어떤 쪽을 못 여는 파일(저장 검증이 거절하므로 왕복 대상이 아님, `--probe` 가 `broken-page` 로 표시),
  암호를 모르는 암호 파일, PDFium 이 처리하지 못하는 입력 — pdf.js `tiling-pattern-tiny-steps*.pdf`(XStep 0.001 렌더 폭탄, 렌더가 끝나지 않음)와
  `jpx_numcomps.pdf`(PDFium chromium/8152 JPX 디코더 CHECK 실패로 프로세스 종료 — 상류 버그, PDFium 판을 올릴 때 다시 확인)
- **qpdf-broken 태그**(7개): 원본부터 qpdf 가 오류로 거절하는 파일(PDFium 만 복구해 여는 손상 파일, qpdf 12 가 모르는 Brotli 필터, qpdf 가 못 여는
  암호 설정). 저장본의 qpdf 오류는 우리 탓이 아니므로 참고만 하고, PDFium 다시 열기·비교까지는 통과해야 한다
- **갱신**: PDFium 판을 올리면 `fetch_corpus.py --probe build/dev/bin/onpdf_cli` 로 `expect` 를 다시 맞춘다. 출처 커밋을 올리면 `SOURCES` 의 SHA 를 바꾸고
  `--check` 가 알려 주는 파일의 sha256 을 다시 잰다. 목록 형식·중복·암호 누락은 스크립트가 실행 전에 거절(exit 2)하고 `CorpusList.IsWellFormed` 도 검사한다

## 갱신 절차
```bash
python3 testdata/gen/make_all.py            # 생성 + README 표 갱신 (qpdf 있으면 AES 파일도)
python3 testdata/gen/make_all.py --check    # CI: 커밋본과 바이트 비교
python3 testdata/gen/fetch_pdfium_samples.py   # PDFium 샘플 다시 받기 (버전 올릴 때)
python3 testdata/gen/fetch_corpus.py        # 왕복 코퍼스 받기 (--check 빠짐 보고, --list 요약, --probe <onpdf_cli> 기대값 실측)
pip install fonttools brotli && python3 testdata/gen/fetch_font.py   # 한글 서브셋 폰트 갱신 (글자 추가 시)
ONPDF_UPDATE_REFERENCES=1 ./build/dev/bin/onpdf_render_tests         # 렌더링 기준 PNG 갱신
```
- 새 생성 문서: `gen/docs.py` 에 `make_*` 추가 → `make_all.py` 의 `GENERATED` 에 등록 → `manifest.tsv` 에 행 추가 → 생성·커밋
- 한글 임베딩 문서에 새 글자를 쓰려면 `fetch_font.py` 의 `TEXT` 에 넣고 서브셋을 다시 만든다
- 손상 파일의 `expect` 는 PDFium 복구 결과를 실측해 적는다 (`onpdf_cli info`)
- `--check` 의 바이트 비교는 Flate 출력이 zlib 구현에 의존한다 (CPython 기본 zlib 기준). zlib-ng 를 쓰는 Python 에서 실패하면 환경 문제이지 데이터 변경이 아니다
