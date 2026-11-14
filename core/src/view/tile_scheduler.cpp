#include "onpdf/core/view/tile_scheduler.h"

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

namespace onpdf::view {

namespace detail {

std::vector<NeededTile> compute_needed(const PageLayout& layout, const Rect& viewport_doc,
                                       const SchedulerOptions& options) {
  std::vector<NeededTile> out;
  std::unordered_set<TileKey, TileKeyHash> seen;
  auto add = [&](TileKey key, engine::Priority prio, bool visible) {
    if (seen.insert(key).second) out.push_back({key, prio, visible});
  };
  const int scale_q = layout.scale_q();
  const std::vector<int> visible_pages = layout.pages_in(viewport_doc);
  if (visible_pages.empty()) return out;

  // 1. 보이는 쪽: 미리보기 + 뷰포트와 교차하는 타일
  for (int page : visible_pages) {
    const engine::PageSize pt = layout.page_size_pt(page);
    add({page, options.preview_scale_q, 0, 0}, engine::Priority::kHigh, true);
    const Rect r = layout.page_rect(page);
    const Rect vis = viewport_doc.intersected(r).translated(-r.x, -r.y);  // 쪽 픽셀 좌표
    if (vis.empty()) continue;
    const int tx0 = static_cast<int>(vis.x) / kTileSize;
    const int ty0 = static_cast<int>(vis.y) / kTileSize;
    const int tx1 = static_cast<int>(std::ceil(vis.right()) - 1) / kTileSize;
    const int ty1 = static_cast<int>(std::ceil(vis.bottom()) - 1) / kTileSize;
    const Size px = page_pixel_size(pt, scale_q);
    const int max_tx = tiles_across(px.w) - 1;
    const int max_ty = tiles_across(px.h) - 1;
    for (int ty = std::max(0, ty0); ty <= std::min(ty1, max_ty); ++ty) {
      for (int tx = std::max(0, tx0); tx <= std::min(tx1, max_tx); ++tx) {
        add({page, scale_q, tx, ty}, engine::Priority::kHigh, true);
      }
    }
  }

  // 2. 앞뒤 쪽 미리 렌더 (타일 수가 적을 때만)
  const int first = visible_pages.front();
  const int last = visible_pages.back();
  std::vector<int> prefetch;
  for (int d = 1; d <= options.prefetch_pages; ++d) {
    if (last + d < layout.page_count()) prefetch.push_back(last + d);
    if (first - d >= 0) prefetch.push_back(first - d);
  }
  for (int page : prefetch) {
    const engine::PageSize pt = layout.page_size_pt(page);
    add({page, options.preview_scale_q, 0, 0}, engine::Priority::kLow, false);
    const Size px = page_pixel_size(pt, scale_q);
    const int nx = tiles_across(px.w);
    const int ny = tiles_across(px.h);
    if (nx * ny > options.max_prefetch_tiles) continue;
    for (int ty = 0; ty < ny; ++ty) {
      for (int tx = 0; tx < nx; ++tx) add({page, scale_q, tx, ty}, engine::Priority::kLow, false);
    }
  }
  return out;
}

}  // namespace detail

struct TileScheduler::State {
  struct InFlight {
    engine::TaskHandle handle;
    engine::Priority priority;
    std::chrono::steady_clock::time_point started;
  };

  TileSource& source;
  TileCache& cache;
  SchedulerOptions options;
  const PageLayout* layout = nullptr;
  std::unordered_map<TileKey, InFlight, TileKeyHash> in_flight;
  std::unordered_set<TileKey, TileKeyHash> failed;
  std::vector<NeededTile> needed;  // 마지막 update 의 필요 집합
  std::function<void(const TileKey&)> on_tile;
  Stats stats;
  double render_ms_total = 0.0;

  State(TileSource& s, TileCache& c, SchedulerOptions o) : source(s), cache(c), options(o) {}

  void cancel_all() {
    for (auto& [key, f] : in_flight) {
      f.handle.cancel();
      ++stats.cancelled;
    }
    in_flight.clear();
  }

  void on_done(const TileKey& key, Result<Bitmap> result) {
    auto it = in_flight.find(key);
    if (it != in_flight.end()) {
      const auto elapsed = std::chrono::steady_clock::now() - it->second.started;
      render_ms_total += std::chrono::duration<double, std::milli>(elapsed).count();
      in_flight.erase(it);
    }
    if (!result) {
      if (result.error().code == ErrorCode::kCancelled) return;
      ++stats.failed;
      failed.insert(key);
      if (on_tile) on_tile(key);
      return;
    }
    ++stats.completed;
    stats.avg_render_ms = render_ms_total / stats.completed;
    cache.put(key, std::make_shared<const Bitmap>(std::move(*result)));
    if (on_tile) on_tile(key);
  }
};

TileScheduler::TileScheduler(TileSource& source, TileCache& cache, SchedulerOptions options)
    : state_(std::make_shared<State>(source, cache, options)) {}

TileScheduler::~TileScheduler() { state_->cancel_all(); }

void TileScheduler::set_layout(const PageLayout* layout) {
  state_->layout = layout;
  state_->failed.clear();
}

void TileScheduler::set_on_tile(std::function<void(const TileKey&)> on_tile) {
  state_->on_tile = std::move(on_tile);
}

void TileScheduler::cancel_all() { state_->cancel_all(); }

void TileScheduler::update(const Rect& viewport_doc) {
  State& s = *state_;
  if (s.layout == nullptr) return;
  s.needed = detail::compute_needed(*s.layout, viewport_doc, s.options);

  // 더는 필요 없는 요청 취소
  std::unordered_map<TileKey, engine::Priority, TileKeyHash> wanted;
  for (const auto& n : s.needed) wanted.emplace(n.key, n.priority);
  for (auto it = s.in_flight.begin(); it != s.in_flight.end();) {
    auto w = wanted.find(it->first);
    const bool keep = w != wanted.end() &&
                      !(w->second == engine::Priority::kHigh && it->second.priority == engine::Priority::kLow);
    if (keep) {
      ++it;
      continue;
    }
    it->second.handle.cancel();  // 우선순위가 올라간 경우도 취소 후 재요청
    ++s.stats.cancelled;
    it = s.in_flight.erase(it);
  }

  // 캐시에도 없고 요청 중도 아닌 타일 요청. 보이는 타일 먼저 넣는다(같은 우선순위 안에서 FIFO)
  std::weak_ptr<State> weak = state_;
  for (const auto& n : s.needed) {
    if (s.cache.contains(n.key) || s.in_flight.contains(n.key) || s.failed.contains(n.key)) continue;
    TileRequest req{n.key, s.layout->page_size_pt(n.key.page)};
    State::InFlight f{{}, n.priority, std::chrono::steady_clock::now()};
    // 동기 소스는 request() 안에서 바로 콜백을 부르므로, 먼저 in_flight 에 넣는다
    s.in_flight.emplace(n.key, std::move(f));
    ++s.stats.requested;
    engine::TaskHandle handle = s.source.request(req, n.priority, [weak](const TileKey& key, Result<Bitmap> r) {
      if (auto st = weak.lock()) st->on_done(key, std::move(r));
    });
    auto again = s.in_flight.find(n.key);
    if (again != s.in_flight.end()) again->second.handle = std::move(handle);
  }
}

int TileScheduler::pending_visible() const noexcept {
  int n = 0;
  for (const auto& t : state_->needed) {
    if (t.visible && !state_->cache.contains(t.key) && !state_->failed.contains(t.key)) ++n;
  }
  return n;
}

int TileScheduler::in_flight() const noexcept { return static_cast<int>(state_->in_flight.size()); }

TileScheduler::Stats TileScheduler::stats() const noexcept { return state_->stats; }

}  // namespace onpdf::view
