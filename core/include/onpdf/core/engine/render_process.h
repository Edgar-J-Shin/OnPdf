#pragma once

// 렌더 자식 프로세스의 부모 쪽 클라이언트 (#137). PDFium 을 부르지 않는 전용 스레드가
// High/Normal/Low 큐를 비우며 자식(`onpdf_app --render-child`)에 요청을 하나씩 보내고 공유 메모리로
// 픽셀을 받는다. 자식이 죽거나(파이프 EOF) 예산을 넘기면(kill) 진행 중 요청을 kRenderProcessLost 로
// 끝내고 자식을 다시 띄워(마지막 Open·Replace 재전송) 큐를 이어 간다. 같은 쪽이 max_page_failures
// 번 실패하면 그 쪽은 kRenderRefused 로 즉시 거절한다. DocumentHandle 이 render_page/render_region
// 을 이쪽으로 보낸다(TaskHandle 취소 의미는 PdfiumWorker 와 같다 — 아직 보내지 않은 요청만).
//
// 편집 뒤 옛 내용을 그리지 않기: begin_resync() 가 세대를 올려 돌려주고, 그 뒤의 렌더 요청은 그
// 세대로 태그된다. replace(generation, bytes) 가 자식에 적용되기 전에는 그 세대의 렌더를 보내지
// 않는다.

#include "onpdf/core/bitmap.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/engine/render_types.h"
#include "onpdf/core/error.h"
#include "onpdf/core/geometry.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace onpdf::engine {

struct RenderProcessImpl;

class RenderProcess {
 public:
  struct Options {
    std::filesystem::path exe;  // 자식 실행 파일 (앱 자신)
    bool use_system_fonts = true;
    std::chrono::milliseconds timeout{20000};          // 렌더 한 건의 예산 — 넘기면 kill
    std::chrono::milliseconds control_timeout{60000};  // Open/Replace 예산 (큰 문서 열기)
    int max_page_failures = 2;                         // 같은 쪽이 이만큼 죽이면 거절
    int max_spawn_failures = 3;                        // 연속 띄우기 실패 → 사용 불가(spawn_failed)
    std::filesystem::path report_dir;  // 자식 크래시 기록 폴더 (환경 ONPDF_REPORT_DIR 로 전달)
  };

  struct Event {
    enum class Kind { kRestarted, kRefused, kSpawnFailed };
    Kind kind = Kind::kRestarted;
    int page = -1;
    std::string reason;  // "crashed" | "timeout" | 메시지
  };
  using EventCallback = std::function<void(const Event&)>;  // 클라이언트 스레드에서 호출

  struct Stats {
    std::uint64_t rendered = 0;
    int restarts = 0;
    bool unavailable = false;  // spawn 실패 누적 — 호출자는 워커 렌더로 폴백
  };

  explicit RenderProcess(Options options);
  ~RenderProcess();  // 큐 비우기(kCancelled 통지) → 자식 종료(EOF → 1초 → kill) → join
  RenderProcess(const RenderProcess&) = delete;
  RenderProcess& operator=(const RenderProcess&) = delete;

  // 문서 지정 (제어 메시지 — 큐 맨 앞). 성공·실패는 이벤트·stats 로만 알린다
  void open(std::filesystem::path path, std::string password);
  void open_from_memory(std::vector<std::uint8_t> bytes, std::string password);
  void close();
  // 편집 재동기화: 세대를 올려 돌려준다. 이후 render_* 는 이 세대로 태그되어 replace 전에는 보류
  [[nodiscard]] std::uint64_t begin_resync();
  void replace(std::uint64_t generation, std::vector<std::uint8_t> bytes);

  TaskHandle render_page(int page_index, const RenderOptions& options, Priority priority,
                         std::function<void(Result<Bitmap>)> on_done);
  TaskHandle render_region(int page_index, const RenderOptions& options, const Rect& region_px,
                           Priority priority, std::function<void(Result<Bitmap>)> on_done);

  void set_on_event(EventCallback callback);
  [[nodiscard]] Stats stats() const;
  [[nodiscard]] bool unavailable() const noexcept;
  [[nodiscard]] const Options& options() const noexcept;

 private:
  std::shared_ptr<RenderProcessImpl> impl_;
};

}  // namespace onpdf::engine
