#pragma once

// 뷰포트에 필요한 타일을 계산해 캐시 미스만 요청하고, 더는 필요 없는 요청을 취소한다.
// 메인 스레드 전용. 콜백은 weak_ptr 로 상태를 잡으므로 스케줄러가 교체·소멸된 뒤 도착한 타일은 무시된다.
// 재진입 금지: on_tile 콜백 안에서 update() 를 부르면 안 된다 (동기 소스에서는 update() 의 반복 중에
// 콜백이 실행된다). 앱의 DocumentTileSource 는 항상 이벤트 루프로 지연하므로 해당 없음.

#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/geometry.h"
#include "onpdf/core/view/page_layout.h"
#include "onpdf/core/view/tile.h"
#include "onpdf/core/view/tile_cache.h"
#include "onpdf/core/view/tile_source.h"

#include <functional>
#include <memory>
#include <vector>

namespace onpdf::view {

struct SchedulerOptions {
  int prefetch_pages = 1;        // 앞뒤로 미리 렌더할 쪽 수
  int preview_scale_q = 8;       // 쪽 전체 미리보기 배율 (0.125 px/pt → A4 ≈ 105px)
  int max_prefetch_tiles = 32;   // 한 쪽의 타일 수가 이보다 많으면 미리 렌더 생략 (고배율)
};

struct NeededTile {
  TileKey key;
  engine::Priority priority;
  bool visible;  // 뷰포트 안(true) / 미리 렌더(false)
};

namespace detail {
// 순수 함수: 뷰포트에 필요한 타일 목록 (보이는 타일 kHigh, 미리보기 kHigh, 프리페치 kLow). 중복 없음
std::vector<NeededTile> compute_needed(const PageLayout& layout, const Rect& viewport_doc,
                                       const SchedulerOptions& options);
}  // namespace detail

class TileScheduler {
 public:
  struct Stats {
    int requested = 0;
    int cancelled = 0;
    int completed = 0;
    int failed = 0;
    double avg_render_ms = 0.0;
  };

  TileScheduler(TileSource& source, TileCache& cache, SchedulerOptions options = {});
  ~TileScheduler();
  TileScheduler(const TileScheduler&) = delete;
  TileScheduler& operator=(const TileScheduler&) = delete;

  // 레이아웃은 뷰가 소유한다. 배율이 바뀌면 다시 호출하지 않아도 되지만(포인터 동일) 실패 집합은 초기화된다
  void set_layout(const PageLayout* layout);
  void update(const Rect& viewport_doc);
  void cancel_all();
  // 타일이 캐시에 들어간 직후(또는 실패 확정 직후) 호출
  void set_on_tile(std::function<void(const TileKey&)> on_tile);

  [[nodiscard]] int pending_visible() const noexcept;
  [[nodiscard]] int in_flight() const noexcept;
  [[nodiscard]] Stats stats() const noexcept;

 private:
  struct State;
  std::shared_ptr<State> state_;
};

}  // namespace onpdf::view
