# 렌더 자식 프로세스 (#137)

PDFium 렌더(타일·썸네일·인쇄·미리보기)는 별도 프로세스에서 돈다. PDFium 이 렌더 중에 죽거나(JPX 디코더 `CHECK`, #125) 끝나지 않아도(타일링 패턴 폭탄, #121) 앱은 살아남고 그 쪽만 "표시할 수 없습니다" 로 둔다. 편집·텍스트 추출·검색·주석 조회·저장은 지금처럼 앱 안의 PDFium 워커가 한다 — 편집 상태(스냅샷·실행 취소 토큰·떼어 둔 객체)를 프로세스 밖으로 옮기지 않기 위해서다(스파이크 #135).

## 프로세스 모델

```
onpdf_app (부모)                                  onpdf_app --render-child <in> <out> (자식)
┌──────────────────────────────┐                 ┌──────────────────────────────┐
│ 메인(Qt)  워커(PDFium 편집)   │   파이프(요청)   │ PdfiumLibrary + PdfiumDocument│
│ RenderProcess 클라이언트 스레드 ├───────────────▶│ render_page / render_region   │
│   High/Normal/Low 큐·취소     │◀───────────────┤ (렌더 위험 캐시·폼 환경 포함)  │
│   예산 감시·재시작            │  파이프(응답)    │                              │
│                              │  공유 메모리(픽셀·문서 바이트)                   │
└──────────────────────────────┘                 └──────────────────────────────┘
```

- **자식 실행 파일은 앱 자신**이다. `main()` 첫 줄에서 `--render-child` 를 보고 QApplication·설정·GUI 를 건드리지 않은 채 `engine::run_render_child` 로 간다. 배포물·서명·설치기·DMG 에 추가 실행 파일이 없다. 시작 비용은 약 40~60ms(Qt 라이브러리 로드 포함).
- **IPC**(`core/platform/ipc.h`): 익명 파이프 2개 + 공유 메모리. POSIX 는 `posix_spawn`·`pipe`·`shm_open`, Windows 는 `CreateProcessW`(핸들 2개만 상속) + `CreateFileMappingW` + Job 객체(`KILL_ON_JOB_CLOSE` — 부모가 죽으면 자식도 끝난다). 플랫폼 분기는 이 파일 안에만.
- **프로토콜**(`core/engine/render_protocol.h`): 고정 24바이트 헤더(`OPRC`, 버전, 종류, 페이로드 길이, 요청 id) + 가변 페이로드. 부모→자식 `Open{경로, 암호, 시스템 글꼴}`·`Replace{세대, 공유 메모리 이름, 바이트}`·`Render{세대, 쪽, 배율, 회전, 플래그, 영역, 공유 메모리}`·`Close`·`Ping`, 자식→부모 `Ok`·`Rendered{w, h, stride, 소요}`·`Error{코드, 메시지}`. 큰 데이터(픽셀·문서 바이트)는 공유 메모리로. 버전이 다르면 자식이 `Error{kUnsupported}` 를 돌려주고 끝난다.

## 수명

1. **열기**: `DocumentSession` 이 워커로 문서를 연 뒤 `DocumentHandle::attach_render_process` 로 자식을 붙인다 → 큐 맨 앞에 `Open`. 자식은 같은 파일을 열고(암호 포함) 렌더 위험을 qpdf 로 검사한다(부모와 같은 판정).
2. **렌더**: `DocumentHandle::render_page/render_region` 이 자식이 붙어 있으면 `RenderProcess` 로 간다. 호출자(`DocumentTileSource`·`ThumbnailPanel`·`PrintJob`·`PrintPreviewView`)는 바뀌지 않았다 — 콜백은 클라이언트 스레드에서 오고 각자 `UiDispatcher::post_to` 로 메인에 넘긴다.
3. **편집 뒤**(`apply_change`): `begin_resync()` 로 세대를 올려 이후 렌더 요청을 보류시키고, 워커가 `save_to_memory` 로 현재 바이트를 만들면 `replace(세대, 바이트)` → 자식이 메모리에서 다시 연다 → 보류된 요청 처리. 옛 바이트로 그린 타일은 뷰에 가지 않는다(`invalidate_pages` 가 스케줄러를 재생성해 늦은 결과를 버린다). 7.9MB 문서의 메모리 저장은 약 65ms.
4. **닫기**: 핸들 소멸 → `Close`. 앱 종료 → 파이프 EOF → 자식 종료(1초 안에 안 끝나면 kill).

## 장애 처리

| 상황 | 감지 | 처리 |
|---|---|---|
| 자식 크래시 | 응답 파이프 EOF | 진행 중 요청 `kRenderProcessLost("crashed")`, 쪽 실패 +1, 재시작(마지막 `Open`·`Replace` 재전송), 이벤트 `restarted` → 상태 표시줄 |
| 렌더 멈춤 | 예산 초과(설정 "한 쪽 렌더링 포기 시간", 기본 20초) | `kill` → 위와 같음(`"timeout"`) |
| 같은 쪽 2회 실패 | `page_failures` | 그 쪽은 `kRenderRefused` 로 즉시 거절 → #121 의 안내·썸네일 "표시 불가"·도구 차단 재사용 |
| 자식 띄우기 3회 연속 실패 | spawn/Open 실패 | `unavailable` → `DocumentHandle` 이 워커 렌더로 폴백, 설정 자동 끔 + 안내 |
| 바이트 생성 실패 | `save_to_memory` 오류 | 그 세대 보류 해제 + 워커 폴백 |

`kRenderProcessLost` 는 **재시도 가능**: `TileScheduler` 는 실패 집합에 넣지 않고 바로 다시 요청하고(`request_missing`), 썸네일·미리보기·인쇄는 한 번 다시 요청한다. 자식의 크래시 기록은 자식 자신의 핸들러(#131)가 보고 폴더에 남긴다(부모가 `ONPDF_REPORT_DIR` 을 넘긴다).

"응답 없음" 창(60초, #121)은 이제 **워커**(편집·저장) 멈춤에만 뜬다.

## 설정·점검·테스트

- 설정 > 고급: "쪽을 별도 프로세스에서 그립니다"(기본 켬, 다음 문서부터), "한 쪽 렌더링 포기 시간"(5~120초).
- `onpdf_app --self-test <pdf>` 는 첫 쪽을 자식에서 그렸는지 확인한다(아니면 종료 코드 4) — 배포물에서 자기 재실행이 되는지 점검.
- 테스트: `onpdf_render_process_tests`(프로토콜·IPC·클라이언트: 우선순위·취소·세대 보류·크래시 재시작·2회 거부·시간 초과·큰 요청·실행 파일 없음), `onpdf_render_tests` 의 `ChildProcessMatchesReferences`(기준 PNG 10개가 자식 경로에서도 픽셀 동일), 앱 `RenderProcessAppTest`(기본 켬 타일·편집 뒤 새 내용·크래시 주입·멈춤 주입·끄기·폴백). 장애 주입은 환경 변수 `ONPDF_RENDER_CHILD_FAULT=crash:<쪽>|hang:<쪽>`(자식이 그 쪽을 그리기 직전에 죽거나 멈춘다). 자식 실행 파일은 각 테스트 바이너리 자신(`main` 분기).

## 측정 (macOS arm64, Debug, 스파이크 #135)

IPC 오버헤드 약 20µs/타일(실제 쪽 1~4%), JPX 크래시 복구 98ms, 렌더 폭탄은 예산 뒤 kill·재시작 14ms.
