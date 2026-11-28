# v1.0 수동 QA 체크리스트 (#117 ②)

v1.0 게이트 #117 의 두 번째 조건 "실기기·타 뷰어 수동 QA" 를 사람이 직접 돌리는 절차다. 자동 테스트가 매 PR·릴리스에서 이미 확인하는 것(아래 "자동으로 확인되는 것")은 넣지 않았다. 각 항목은 **절차 → 기대 결과** 이고, 결과는 맨 끝 "기록" 표에 적는다.

**통과 기준**: A~G 의 모든 항목이 두 플랫폼(해당하는 쪽)에서 `통과`, 또는 `실패` 가 전부 이슈로 올라가 있고 v1.0 차단(blocking)으로 판정된 것이 없을 때 #117 ② 를 체크한다. 서명·공증(#117 ①)이 켜진 **그 빌드**로 돈다 — 서명 전 빌드로 통과한 항목은 B-3·B-4·A-7 을 다시 한다.

## 준비물

| 종류 | 필요한 것 | 비고 |
|---|---|---|
| Windows 기기 | Windows 10 x64 1대, Windows 11 x64 1대 (VM 가능) | 그중 하나는 **한글 사용자 이름 계정**(예: `홍길동`)으로 로그인 — 경로 `C:\Users\홍길동\…` |
| Mac 기기 | Intel Mac + macOS 13 (최소 지원 판), Apple Silicon Mac (최신 macOS) | Intel 이 없으면 Apple Silicon 에서 x64 DMG 를 Rosetta 로 — 기록에 "Rosetta" 라고 적는다 |
| 프린터 | 실제 프린터 1대(양면 가능하면 좋다) + PDF 프린터(Microsoft Print to PDF / macOS "PDF 로 저장") | |
| 타 뷰어 | Adobe Acrobat Reader(최신), Chrome, Edge(Windows), macOS 미리보기 | 저장본 표시 비교용 |
| 배포물 | 같은 릴리스의 `…-windows-x64-setup.exe`, `…-windows-x64.zip`, `…-macos-arm64.dmg`, `…-macos-x64.dmg`, `SHA256SUMS.txt` | 받은 뒤 `SHA256SUMS.txt` 와 대조(`certutil -hashfile … SHA256` / `shasum -a 256 …`) |
| 테스트 PDF | 저장소 `testdata/` 의 아래 파일 + **실제 문서 2개 이상**(스캔본 1, 한글 텍스트 문서 1) | 실제 문서는 라이선스 무관 — 저장소에 넣지 않는다 |

쓰는 testdata 파일(`python3 testdata/gen/make_all.py` 또는 저장소 체크아웃):
`simple.pdf`, `korean-embedded.pdf`, `korean-system-font.pdf`, `text-lines-basic.pdf`, `text-lines-type3.pdf`, `text-lines-kern.pdf`, `form-fields.pdf`, `annotations.pdf`, `bookmarks.pdf`, `large-1000.pdf`, `mixed-sizes.pdf`, `scanned.pdf`, `encrypted-aes256.pdf`(암호 `user`), `print-flags.pdf`, `print-low-res.pdf`, `signed-dummy.pdf`, `pattern-bomb.pdf`, `truncated.pdf`, `한글 파일명.pdf`. JPX 크래시 재현에는 pdf.js 의 `jpx_numcomps.pdf`(코퍼스 밖, #125).

## 자동으로 확인되는 것 (수동 QA 에서 뺀다)

- 배포물 `--self-test`(첫 쪽 렌더·번들 글꼴·qpdf·렌더 자식 자기 재실행, 한글 경로) — 릴리스 워크플로우가 4종 모두 돌린다
- Windows 설치기 리허설: 조용한 설치 → 설치본 self-test → 레지스트리(ProgId·OpenWithProgids·Applications) → 조용한 제거 → 폴더·키 잔여 없음
- macOS 번들의 아키텍처·minos ≤ 13.0·`LSMinimumSystemVersion`·번들 밖 의존 없음
- 렌더 회귀(기준 PNG, 자식 경로 포함), 왕복 코퍼스 260개, 단위·앱 테스트 206개, 번역 누락(`i18n_check`)

## A. Windows 설치·제거 (Windows 10 과 11 각각)

| ID | 절차 | 기대 결과 |
|---|---|---|
| A-1 | `…-setup.exe` 두 번 클릭 → 기본값(현재 사용자만)으로 설치 | UAC 없이 설치. 시작 메뉴에 OnPdf. 첫 화면에 "서명 없음" 안내가 **없다**(서명 빌드). 설치 폴더 `%LocalAppData%\Programs\OnPdf` |
| A-2 | 제거 뒤 다시 설치하되 "모든 사용자" 선택 | UAC 가 뜨고 `C:\Program Files\OnPdf` 에 설치. 다른 계정으로 로그인해도 시작 메뉴에 보인다 |
| A-3 | 탐색기에서 `.pdf` 오른쪽 클릭 → 연결 프로그램 | 목록에 **OnPdf** 가 있고 고르면 그 파일이 열린다. 아이콘이 OnPdf 아이콘 |
| A-4 | 제거 → 설치 때 "PDF 파일을 OnPdf 로 열기" 작업을 **켜고** 설치 → `.pdf` 두 번 클릭 | OnPdf 로 열리거나, Windows 가 기본 앱 확인(UserChoice)을 요청해 OnPdf 를 고르면 열린다(작업 설명이 "확인을 요청할 수 있습니다" 라고 말한다). 설정 → 앱 → 기본 앱에서 `.pdf` 에 OnPdf 가 후보로 보인다 |
| A-5 | 이전 판(예: v0.8.x)을 먼저 설치 → 그 위에 새 판 설치 → 설치 폴더 비교 | 이전 판이 조용히 제거된 뒤 설치되어 **이전 판에만 있던 파일이 남지 않는다**. 설정(최근 파일·창 위치)은 유지된다(레지스트리 `HKCU\Software\OnPdf\OnPdf`) |
| A-6 | 설정 → 앱 → 설치된 앱에서 OnPdf 제거 | 폴더·시작 메뉴·`HKCU\Software\Classes\OnPdf.Document` 가 사라지고, 다른 프로그램의 `.pdf` 연결은 건드리지 않는다. 사용자 설정 키는 남아도 된다(의도) |
| A-7 | 한글 사용자 이름 계정에서 A-1·A-3 반복 + `C:\Users\홍길동\Documents\한글 파일명.pdf` 열기·저장·인쇄 미리보기 | 전부 정상. 충돌 보고 폴더(`%LocalAppData%\OnPdf\OnPdf\crash-reports`)도 한글 경로에서 동작 |
| A-8 | `…-setup.exe`·zip 의 `onpdf_app.exe` 속성 → 디지털 서명 | 서명자 이름·타임스탬프가 보인다. 두 번 클릭 때 SmartScreen "알 수 없는 게시자" 가 **아니다**(평판이 쌓이기 전이면 경고가 뜰 수 있다 — 서명자 이름이 맞으면 통과로 기록하고 비고에 적는다) |
| A-9 | zip 을 바탕화면에 풀고 `onpdf_app.exe` 실행, PDF 를 exe 위로 끌어 놓기 | 설치 없이 열린다. 작업 관리자에 `onpdf_app.exe` 둘(부모 + `--render-child`) |

## B. macOS 설치·실행 (Apple Silicon + Intel/macOS 13)

| ID | 절차 | 기대 결과 |
|---|---|---|
| B-1 | arm64 DMG 열기 → OnPdf.app 을 Applications 로 끌기 → 두 번 클릭 | Control-클릭 우회 없이 바로 열린다(공증 빌드). 첫 실행 "인터넷에서 받은 앱" 확인만 |
| B-2 | **Intel Mac + macOS 13.x** 에서 x64 DMG 로 B-1 반복 | 열리고 `simple.pdf`·`korean-embedded.pdf` 렌더 정상. "이 Mac 에서 열 수 없음" 이 뜨면 실패 |
| B-3 | 터미널에서 `docs/signing.md` "검증" 의 `codesign -dv --verbose=4`, `spctl --assess --type execute -vv`, `xcrun stapler validate` | Developer ID Application, `runtime` 플래그, `accepted source=Notarized Developer ID`, staple OK |
| B-4 | 앱을 띄우고 활동 상태 보기에서 프로세스 이름 `onpdf_app` 검색 → `jpx_numcomps.pdf` 열기 | 프로세스가 **둘**(부모 + 자식, 인수 `--render-child`). JPX 파일: 앱은 살아 있고 1초 안에 상태 표시줄 "렌더 프로세스가 비정상 종료되어 다시 시작" 뒤 그 쪽만 "표시할 수 없습니다". hardened runtime 에서 자기 재실행이 막히면 "별도 렌더 프로세스를 시작할 수 없어…" 가 뜨고 설정이 꺼진다 — **실패**로 기록(서명 옵션 수정 필요) |
| B-5 | 앱 종료 뒤 활동 상태 보기 다시 확인 | `--render-child` 프로세스가 남지 않는다 |
| B-6 | Retina 와 외장(1x) 모니터 사이로 창을 옮기며 스크롤·확대 | 흐릿한 타일이 금방 선명한 타일로 바뀌고 썸네일도 다시 그려진다 |
| B-7 | 시스템 설정 → 일반 → 언어를 영어로 → 앱 재실행 | 메뉴·대화상자가 영어. 한국어로 되돌리면 한국어(F 절) |

## C. 기능 스모크 (두 플랫폼 각각, 설치본으로)

각 항목은 한 번씩만 돌린다. 세부 동작은 자동 테스트가 덮으므로 **눈으로 보이는 결과**만 확인한다.

| ID | 절차 | 기대 결과 |
|---|---|---|
| C-1 | `large-1000.pdf` 열기 → 끝까지 빠르게 스크롤, 확대·축소, 쪽 번호 입력 이동 | 첫 화면 1초 안. 빈 화면 없이 미리보기 → 선명한 타일. 끊김 없음 |
| C-2 | `bookmarks.pdf` 사이드바(F4) 목차·썸네일 클릭, 검색(Cmd/Ctrl+F)으로 한글 단어 | 목차 이동·현재 항목 강조, 썸네일 현재 쪽 강조, 검색 결과 강조·순환 |
| C-3 | `annotations.pdf`: 하이라이트·밑줄·메모·펜·사각형·원 각 하나 추가, 색·두께·불투명도 변경, 삭제, 실행 취소·다시 실행 | 모두 즉시 반영. 메모 내용은 툴팁·대화상자. 저장 뒤 다시 열어도 같다 |
| C-4 | `simple.pdf` 에 텍스트 도구로 **한글 + 영문 문장** 추가(크기 10.5 pt 직접 입력, 색 변경), 이동·폭 조절, 두 번 클릭 재수정 | 번들 Noto Sans KR 로 그려진다. 재선택·재수정이 된다(#141) |
| C-5 | `text-lines-basic.pdf`·`text-lines-type3.pdf`: 텍스트 도구로 기존 줄 두 번 클릭 → 글 바꾸기 → 크기·색 바꾸기 → 줄 삭제 → 실행 취소 | 줄 밖은 그대로. Type3 줄은 대체 글꼴 안내. 자간 맞춤으로 폭이 비슷 |
| C-6 | 이미지 추가(PNG 투명·JPEG), 크기 조절·이동·삭제 | 투명 유지, JPEG 재압축 없음(파일 크기), 기존 이미지는 선택만 되고 이동 시 안내 |
| C-7 | `form-fields.pdf`: 텍스트·여러 줄·체크·라디오·콤보·목록 입력, Tab 이동, 한글 입력(IME) | 값이 저장되고 다시 열어도 유지. 서명 칸·읽기 전용은 안내만 |
| C-8 | 썸네일에서 쪽 회전·여러 쪽 선택 삭제·끌어서 이동·파일 끌어 놓기 삽입(암호 파일 `encrypted-aes256.pdf` 포함) | 즉시 반영, 암호 입력 창, 실행 취소로 복원 |
| C-9 | 병합(3개, 첫 파일 목차 유지)·분할(3쪽마다, 덮어쓰기 확인) | 결과 파일이 열리고 목차 유지, 분할 파일 수·쪽 수 맞음 |
| C-10 | 저장(Cmd/Ctrl+S), 다른 이름으로 저장, 읽기 전용 파일에 저장 시도, 저장 중 닫기 | 원본 교체는 임시 파일 뒤 교체. 읽기 전용은 "다른 이름으로 저장" 안내. 수정 표시(제목 `*`) |
| C-11 | 설정(일반·편집·고급) 바꾸고 재실행 | 기본 두께·색·크기·배율·사이드바·창 위치 복원. 언어 변경은 재시작 안내 |
| C-12 | `truncated.pdf`·`not_a_pdf.txt` 열기, `encrypted-aes256.pdf` 틀린 암호 → 맞는 암호 | 오류 안내 뒤 앱 정상. 암호 재질문 |
| C-13 | 앱을 강제 종료(작업 관리자/활동 상태 보기) → 재실행 | "지난번에 … 를 여는 중에 비정상 종료" 안내(다시 열기/열지 않기/보고 폴더/이슈). 보고 폴더에 기록 파일 |

## D. 인쇄 (실제 프린터)

| ID | 절차 | 기대 결과 |
|---|---|---|
| D-1 | `mixed-sizes.pdf` 인쇄 → 미리보기에서 **맞춤 / 실제 크기 / 큰 쪽만 축소** 를 바꿔 보며 각 모드로 1장씩 출력 | 출력물의 배치(여백·잘림·자동 회전)가 미리보기와 같다. A5·명함 쪽은 실제 크기에서 가운데, 큰 쪽은 축소 |
| D-2 | `print-flags.pdf` 인쇄 대화상자 열기 | 매수 2·쪽 범위 2쪽·양면이 미리 채워져 있고(/ViewerPreferences), 화면 전용 주석은 출력에 **없고** 인쇄 전용 주석은 **있다** |
| D-3 | `print-low-res.pdf` 와 `korean-embedded.pdf` 출력 | 저해상도(150dpi) 는 글자가 흐릿하지만 읽히고, 한글은 선명 |
| D-4 | `annotations.pdf` 흑백으로 출력 | 하이라이트가 회색으로 겹쳐 글자가 읽힌다 |
| D-5 | 인쇄 금지 문서(`perm-*` 중 인쇄 비트 없는 것을 만들거나 실제 문서)에서 Cmd/Ctrl+P | "권한 암호" 안내 → 소유자 암호 입력 → 인쇄 가능. 사용자 암호를 넣으면 "권한 암호가 아닙니다" |
| D-6 | `large-1000.pdf` 를 PDF 프린터로 전체 인쇄, 진행 중 다른 문서 열기 시도 | 진행 창(비모달), "인쇄 취소하고 열기 / 계속" 질문. 완료 뒤 PDF 1,000쪽 |

## E. 타 뷰어 표시 (저장본을 Adobe Reader·Chrome·Edge·미리보기에서)

**저장본 만들기**(두 플랫폼 중 한쪽에서 만들고 양쪽 뷰어로 본다): 아래 편집을 각각 한 파일에 하고 다른 이름으로 저장한다.

| 파일 | 편집 | 뷰어에서 볼 것 |
|---|---|---|
| E-1 `text-lines-basic.pdf` → `e1.pdf` | 기존 줄 두 개 수정(하나는 더 길게), 하나 삭제 | 고친 줄의 글꼴·굵기·위치가 OnPdf 와 같다. 지운 줄 자리가 비어 있다. 다른 줄·도형 변화 없음. **검색·복사**가 된다(글자 사이 공백이 끼지 않음, #79) |
| E-2 `simple.pdf` → `e2.pdf` | 한글+영문 텍스트 상자 2개 추가(번들 글꼴), 하나는 회전 쪽 | 한글이 깨지지 않고 같은 위치. 텍스트 선택·복사가 한글로 나온다(ToUnicode) |
| E-3 `annotations.pdf` → `e3.pdf` | 하이라이트·밑줄·메모(한글 내용)·펜·사각형(채움 50%)·원 추가, 기존 하이라이트 색 변경 | Adobe Reader 에서 **같은 모양·색·불투명도**, 메모는 아이콘 + 팝업 내용. 인쇄 플래그: Reader 의 인쇄 미리보기에 주석이 보인다 |
| E-4 `text-lines-type3.pdf` → `e4.pdf` | Type3 줄과 한글 줄을 수정 → 대체 글꼴 선택에서 **시스템 OTF(CFF) 글꼴**(macOS: Apple SD Gothic Neo·Noto Sans CJK OTF) 을 고른다 | 저장 때 "글꼴 변환" 후처리(`FontFile3/OpenType`). Adobe Reader·미리보기(CoreGraphics)에서 **글자가 보인다**(빈 네모·물음표면 실패 — CFF charset 문제) |
| E-5 `form-fields.pdf` → `e5.pdf` | 모든 칸 입력(한글 포함), 체크·라디오·콤보 | Adobe Reader 에서 값이 같고 한글 칸이 보인다(비임베딩 Adobe-Korea1 — Reader 가 글꼴 팩을 요구할 수 있다, 비고에 적는다). Chrome·Edge 에서도 값 표시 |
| E-6 `bookmarks.pdf` → `e6.pdf` | 2쪽 회전, 5쪽 삭제, `simple.pdf` 끝에 삽입, 3쪽을 맨 앞으로 이동 | 쪽 순서·회전이 같고 목차가 살아 있다(지운 쪽 항목은 비활성) |
| E-7 `scanned.pdf` → `e7.pdf` | PNG(투명)·JPEG 이미지 추가, 크기 조절 | 투명 영역이 아래 스캔 이미지를 비친다. JPEG 화질 그대로 |
| E-8 `signed-dummy.pdf` → `e8.pdf` | 하이라이트 하나 추가 → 저장 | Adobe Reader 서명 패널: "문서가 서명 뒤 수정됨" 이되 **서명이 깨지지 않음**(증분 저장). 원본 바이트가 앞부분에 그대로(`cmp -n <원본 크기>`) |

각 저장본은 `qpdf --check e*.pdf`(또는 `onpdf_cli check`) 가 오류 0 이어야 한다.

## F. 언어

| ID | 절차 | 기대 결과 |
|---|---|---|
| F-1 | 시스템 언어 한국어 → 앱 | 메뉴·툴바·대화상자·상태 표시줄·설정 전부 한국어. Qt 표준 버튼(예/아니요/취소, 파일 대화상자)도 한국어 |
| F-2 | 시스템 언어 영어 → 앱 | 전부 영어(원문). 복수형이 1/2 에서 올바름 — 인쇄 미리보기 "1 sheet"/"2 sheets", 분할 덮어쓰기 확인 "1 file"/"2 files" |
| F-3 | 설정 > 일반 > 언어를 반대로 바꾸기 → 재시작 안내 → 재실행 | 바뀐 언어로 뜬다. `--lang ko|en` 인수가 설정보다 우선 |
| F-4 | 한국어 UI 에서 긴 문구가 잘리는 곳 찾기(설정 대화상자·인쇄 미리보기·충돌 안내) | 잘림·겹침 없음(있으면 스크린샷과 함께 이슈) |

## G. 악성·극단 입력

| ID | 절차 | 기대 결과 |
|---|---|---|
| G-1 | `pattern-bomb.pdf` 열기 → 2~4쪽으로 | 열 때 비모달 안내 1회, 2~4쪽은 "이 쪽은 표시할 수 없습니다", 썸네일 "표시 불가", 그 쪽에서 도구 비활성. 앱 응답 유지 |
| G-2 | `jpx_numcomps.pdf` 열기(B-4 와 같음, Windows 에서도) | 앱 생존, 그 쪽만 거부, 보고 폴더에 자식 크래시 기록 2개 |
| G-3 | 설정 > 고급에서 "별도 프로세스" 끄기 → 다음 문서부터 `pattern-bomb.pdf` | 사전 검사로 여전히 거부(자식 없이도 안전). JPX 파일은 이 모드에서 앱이 종료될 수 있다 — **의도된 한계**, 다음 실행 때 충돌 안내가 뜨는지 확인 |
| G-4 | `large-1000.pdf` 를 열어 둔 채 메모리 사용 확인(활동 상태 보기/작업 관리자), 10분 스크롤·확대 | 타일 캐시 상한(설정값, 기본 256MB) 근처에서 안정. 자식 프로세스 메모리도 폭주하지 않는다 |
| G-5 | 경로에 한글·공백·이모지가 든 폴더의 PDF 열기·저장·분할 | 전부 정상(Windows 비ASCII 경로 포함) |

## 기록

아래 표를 복사해 `qa-results/<버전>-<날짜>.md`(저장소 밖 또는 이슈 #117 댓글)에 채운다. 결과는 `통과` / `실패(#이슈)` / `해당 없음` / `보류(사유)`.

| ID | Windows 10 | Windows 11 | macOS 13 Intel | macOS Apple Silicon | 비고 |
|---|---|---|---|---|---|
| A-1 … A-9 | | | — | — | |
| B-1 … B-7 | — | — | | | |
| C-1 … C-13 | | | | | |
| D-1 … D-6 | | | | | 프린터 모델: |
| E-1 … E-8 | Reader / Chrome / Edge | | 미리보기 / Reader / Chrome | | |
| F-1 … F-4 | | | | | |
| G-1 … G-5 | | | | | |

실패는 `bug` 라벨 이슈로 올린다 — 제목 접두 `QA:`, 본문에 체크리스트 ID·배포물 파일 이름·OS 판·재현 파일(저장소 파일이면 이름, 실제 문서면 특징만)·스크린샷. 앱 충돌이면 보고 폴더의 `crash-*.txt`(와 Windows `.dmp`)를 붙인다 — 파일 경로는 기록에 들어가지 않는다.

## 관련 문서
- 서명·공증 설정과 검증 명령: [signing.md](signing.md)
- 렌더 자식 프로세스의 장애 처리 표: [render-process.md](render-process.md)
- 배포 안내문(사용자가 보는 것): `packaging/README-windows.txt`, `packaging/README-macos.txt`
