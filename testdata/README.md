# testdata

테스트에 쓰는 PDF 와 기준 이미지. **라이선스가 명확한 파일만** 넣는다.

| 파일 | 출처 | 라이선스 | 용도 |
|---|---|---|---|
| `simple.pdf` | 이 저장소에서 직접 생성 (`make_simple_pdf.py`) | Apache-2.0 (프로젝트와 동일) | 열기·크기·렌더링 기본 검증. 2쪽(세로/가로), 내장 Helvetica |
| `not_a_pdf.txt` | 직접 작성 | Apache-2.0 | 잘못된 형식 처리 |
| `expected/*.png` | `simple.pdf` 를 렌더링 | Apache-2.0 | 렌더링 회귀 기준 이미지 |

기준 이미지 갱신: `ONPDF_UPDATE_REFERENCES=1 ./build/dev/bin/onpdf_render_tests`

외부 PDF(PDFium·pdf.js 테스트 모음 등)를 추가할 때는 파일별 출처와 라이선스를 이 표에 적는다.
