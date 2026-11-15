#include "onpdf/core/view/tile_scheduler.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <vector>

namespace onpdf::view {
namespace {

// 요청을 기록만 하고, 테스트가 complete()/fail() 로 완료시키는 가짜 소스
class FakeTileSource : public TileSource {
 public:
  struct Pending {
    TileRequest request;
    engine::Priority priority;
    TileCallback on_done;
    std::shared_ptr<std::atomic<bool>> cancelled;  // 스케줄러가 cancel() 하면 true
  };
  engine::TaskHandle request(const TileRequest& r, engine::Priority p, TileCallback cb) override {
    auto flag = std::make_shared<std::atomic<bool>>(false);
    // 같은 키 재요청(승격): 이전 요청은 취소됐어도 이미 실행 중일 수 있으므로 콜백을 보관해 둔다
    if (auto it = pending.find(r.key); it != pending.end()) superseded.push_back(std::move(it->second));
    pending[r.key] = Pending{r, p, std::move(cb), flag};
    ++requested;
    return engine::TaskHandle::for_testing(flag);
  }
  bool is_cancelled(const TileKey& key) const { return pending.at(key).cancelled->load(); }
  void complete(const TileKey& key) {
    auto it = pending.find(key);
    ASSERT_NE(it, pending.end()) << "not pending";
    const Rect rect = tile_rect_px(key, it->second.request.page_pt);
    Bitmap b;
    b.width = static_cast<int>(rect.w);
    b.height = static_cast<int>(rect.h);
    b.stride = b.width * 4;
    b.pixels.assign(static_cast<std::size_t>(b.stride) * static_cast<std::size_t>(b.height), 255);
    auto cb = std::move(it->second.on_done);
    pending.erase(it);
    cb(key, std::move(b));
  }
  void fail(const TileKey& key) {
    auto cb = std::move(pending.at(key).on_done);
    pending.erase(key);
    cb(key, make_error(ErrorCode::kRenderFailed, "boom"));
  }
  // 지난(승격으로 대체된) 요청을 지금 완료시킨다 — 취소 플래그와 무관하게 워커가 이미 렌더한 경우
  void complete_superseded(const TileKey& key) {
    auto it = std::find_if(superseded.begin(), superseded.end(), [&](const Pending& p) { return p.request.key == key; });
    ASSERT_NE(it, superseded.end()) << "no superseded request for key";
    Pending old = std::move(*it);
    superseded.erase(it);
    const Rect rect = tile_rect_px(old.request.key, old.request.page_pt);
    Bitmap b;
    b.width = static_cast<int>(rect.w);
    b.height = static_cast<int>(rect.h);
    b.stride = b.width * 4;
    b.pixels.assign(static_cast<std::size_t>(b.stride) * static_cast<std::size_t>(b.height), 255);
    old.on_done(old.request.key, std::move(b));
  }
  std::map<TileKey, Pending> pending;
  std::vector<Pending> superseded;
  int requested = 0;
};

std::vector<engine::PageSize> letters(int n) { return std::vector<engine::PageSize>(static_cast<std::size_t>(n), {612, 792}); }

class TileSchedulerTest : public ::testing::Test {
 protected:
  TileSchedulerTest() : layout_(letters(10), {12, 16}), scheduler_(source_, cache_, options()) {
    layout_.set_scale_q(64);  // 1 px/pt → 쪽 612×792 → 2×2 타일
    scheduler_.set_layout(&layout_);
    scheduler_.set_on_tile([&](const TileKey& k) { ready_.push_back(k); });
  }
  static SchedulerOptions options() {
    SchedulerOptions o;
    o.prefetch_pages = 1;
    o.preview_scale_q = 8;
    return o;
  }
  int count_priority(engine::Priority p) const {
    return static_cast<int>(std::count_if(source_.pending.begin(), source_.pending.end(),
                                          [&](const auto& kv) { return kv.second.priority == p; }));
  }

  FakeTileSource source_;
  TileCache cache_;
  PageLayout layout_;
  TileScheduler scheduler_;
  std::vector<TileKey> ready_;
};

TEST_F(TileSchedulerTest, ComputeNeededCoversVisibleTilesPreviewAndPrefetch) {
  // 뷰포트: 첫 쪽 왼쪽 위 300×300 → 타일 (0,0) 만 + 미리보기 + 다음 쪽 전체(4타일 + 미리보기)
  const auto needed = detail::compute_needed(layout_, {0, 0, 300, 300}, options());
  int visible_high = 0, prefetch_low = 0;
  for (const auto& n : needed) {
    if (n.visible) {
      EXPECT_EQ(n.priority, engine::Priority::kHigh);
      ++visible_high;
    } else {
      EXPECT_EQ(n.priority, engine::Priority::kLow);
      ++prefetch_low;
    }
  }
  EXPECT_EQ(visible_high, 2);   // 미리보기 + 타일(0,0)
  EXPECT_EQ(prefetch_low, 5);   // 다음 쪽 미리보기 + 2×2
}

TEST_F(TileSchedulerTest, RequestsOnlyMissingTilesAndCompletesIntoCache) {
  scheduler_.update({0, 0, 300, 300});
  EXPECT_EQ(source_.requested, 7);
  EXPECT_EQ(count_priority(engine::Priority::kHigh), 2);
  EXPECT_EQ(scheduler_.pending_visible(), 2);
  source_.complete({0, 64, 0, 0});
  EXPECT_TRUE(cache_.contains({0, 64, 0, 0}));
  EXPECT_EQ(ready_.size(), 1u);
  EXPECT_EQ(scheduler_.pending_visible(), 1);
  // 같은 뷰포트로 다시 update 해도 중복 요청 없음
  scheduler_.update({0, 0, 300, 300});
  EXPECT_EQ(source_.requested, 7);
}

TEST_F(TileSchedulerTest, MovingViewportCancelsObsoleteRequests) {
  scheduler_.update({0, 0, 300, 300});
  ASSERT_EQ(scheduler_.in_flight(), 7);
  // 5쪽으로 점프: 이전 요청은 모두 취소되고 새 집합만 남는다
  const Rect page5 = layout_.page_rect(5);
  scheduler_.update({page5.x, page5.y, 300, 300});
  EXPECT_EQ(scheduler_.stats().cancelled, 7);
  // 이전 뷰포트의 7개 요청은 실제로 취소 표시되어야 한다
  EXPECT_TRUE(source_.is_cancelled({0, 64, 0, 0}));
  EXPECT_TRUE(source_.is_cancelled({0, 8, 0, 0}));
  EXPECT_TRUE(source_.is_cancelled({1, 64, 1, 1}));
  // 새 집합: 5쪽 미리보기+타일(0,0) = 2, 4·6쪽 각각 미리보기+2×2 타일 = 10 → 12
  EXPECT_EQ(scheduler_.in_flight(), 12);
  EXPECT_EQ(source_.requested, 7 + 12);
  EXPECT_FALSE(source_.is_cancelled({5, 64, 0, 0}));
  // 취소된 요청이 늦게 완료돼도 (가짜 소스는 아직 콜백을 들고 있음) 캐시에는 들어가되 통계만
  source_.complete({0, 64, 0, 0});
  EXPECT_TRUE(cache_.contains({0, 64, 0, 0}));
}

TEST_F(TileSchedulerTest, PrefetchPromotedToVisibleIsReRequestedHigh) {
  scheduler_.update({0, 0, 300, 300});
  const TileKey next_tile{1, 64, 0, 0};
  EXPECT_EQ(source_.pending.at(next_tile).priority, engine::Priority::kLow);
  const auto low_flag = source_.pending.at(next_tile).cancelled;
  const Rect p1 = layout_.page_rect(1);
  scheduler_.update({p1.x, p1.y, 300, 300});
  EXPECT_EQ(source_.pending.at(next_tile).priority, engine::Priority::kHigh);
  EXPECT_TRUE(low_flag->load()) << "promoted request must cancel the old kLow task";
  EXPECT_FALSE(source_.is_cancelled(next_tile));
}

TEST_F(TileSchedulerTest, FailedTileIsNotRequestedAgainUntilLayoutReset) {
  scheduler_.update({0, 0, 300, 300});
  source_.fail({0, 64, 0, 0});
  EXPECT_EQ(scheduler_.stats().failed, 1);
  EXPECT_EQ(ready_.size(), 1u);  // 실패도 통지 (빈 타일 표시용)
  scheduler_.update({0, 0, 300, 300});
  EXPECT_FALSE(source_.pending.contains({0, 64, 0, 0}));
  EXPECT_EQ(scheduler_.pending_visible(), 1);  // 실패한 타일은 대기로 세지 않음
  scheduler_.set_layout(&layout_);  // 배율 변경 등 → 실패 집합 초기화
  scheduler_.update({0, 0, 300, 300});
  EXPECT_TRUE(source_.pending.contains({0, 64, 0, 0}));
}

TEST_F(TileSchedulerTest, LateCallbackAfterSchedulerDestroyedIsIgnored) {
  TileCallback cb;
  {
    TileScheduler local(source_, cache_, options());
    local.set_layout(&layout_);
    local.update({0, 0, 300, 300});
    cb = source_.pending.at({0, 64, 0, 0}).on_done;
  }
  Bitmap b;
  b.width = b.height = 1;
  b.stride = 4;
  b.pixels.assign(4, 0);
  cb({0, 64, 0, 0}, std::move(b));  // 소멸 후 도착 → 무시, 크래시 없음
  EXPECT_FALSE(cache_.contains({0, 64, 0, 0}));
}

TEST_F(TileSchedulerTest, HighZoomSkipsPrefetchTiles) {
  layout_.set_scale_q(64 * 8);  // 8 px/pt → 쪽 4896×6336 → 10×13 = 130 타일 > 32
  scheduler_.set_layout(&layout_);
  scheduler_.update({0, 0, 300, 300});
  for (const auto& [key, p] : source_.pending) {
    if (!(key.page == 0)) EXPECT_EQ(key.scale_q, 8) << "prefetch page should only get a preview";
  }
}

// 승격으로 취소한 kLow 요청이 이미 렌더 중이었다가 먼저 끝나면: 그 결과를 쓰고 중복 kHigh 요청은 취소한다.
// 새 요청의 진행 기록을 지우거나 렌더 시간 표본을 어긋나게 하지 않는다 (#12)
TEST_F(TileSchedulerTest, SupersededResultIsUsedAndDuplicateCancelled) {
  scheduler_.update({0, 0, 300, 300});
  const TileKey next_tile{1, 64, 0, 0};
  const Rect p1 = layout_.page_rect(1);
  scheduler_.update({p1.x, p1.y, 300, 300});  // 승격: kLow 취소 + kHigh 재요청
  ASSERT_GE(source_.superseded.size(), 1u);  // 다음 쪽 타일 + 미리보기가 함께 승격된다
  const auto high_flag = source_.pending.at(next_tile).cancelled;
  const int in_flight_before = scheduler_.in_flight();
  const auto completed_before = scheduler_.stats().completed;

  source_.complete_superseded(next_tile);  // 지난 kLow 결과가 먼저 도착
  EXPECT_TRUE(cache_.contains(next_tile));
  EXPECT_TRUE(high_flag->load()) << "duplicate kHigh request must be cancelled";
  EXPECT_EQ(scheduler_.in_flight(), in_flight_before - 1);
  EXPECT_EQ(scheduler_.stats().completed, completed_before + 1);
  EXPECT_DOUBLE_EQ(scheduler_.stats().avg_render_ms, 0.0);  // 지난 요청은 시간 표본에 넣지 않는다
  EXPECT_EQ(std::count(ready_.begin(), ready_.end(), next_tile), 1);

  // 취소된 kHigh 의 늦은 결과가 와도 무시된다 (이미 캐시에 있음, in_flight 변화 없음)
  source_.complete(next_tile);
  EXPECT_EQ(scheduler_.in_flight(), in_flight_before - 1);
  EXPECT_EQ(std::count(ready_.begin(), ready_.end(), next_tile), 1);
  // 다시 update 해도 재요청하지 않는다
  const int requested = source_.requested;
  scheduler_.update({p1.x, p1.y, 300, 300});
  EXPECT_EQ(source_.requested, requested);
}

// 지난 요청 결과가 새 요청 뒤에 오면 무시하고, 렌더 시간 표본은 새 요청만 센다
TEST_F(TileSchedulerTest, LateSupersededResultIgnoredAfterCurrentCompletes) {
  scheduler_.update({0, 0, 300, 300});
  const TileKey next_tile{1, 64, 0, 0};
  const Rect p1 = layout_.page_rect(1);
  scheduler_.update({p1.x, p1.y, 300, 300});
  const int in_flight_before = scheduler_.in_flight();
  source_.complete(next_tile);  // 새 kHigh 먼저
  EXPECT_EQ(scheduler_.in_flight(), in_flight_before - 1);
  const auto completed = scheduler_.stats().completed;
  source_.complete_superseded(next_tile);  // 늦은 지난 결과
  EXPECT_EQ(scheduler_.stats().completed, completed);
  EXPECT_EQ(scheduler_.in_flight(), in_flight_before - 1);
  EXPECT_EQ(std::count(ready_.begin(), ready_.end(), next_tile), 1);
}

// 4,096pt 보다 큰 포스터 쪽은 미리보기 배율을 낮춰 타일 하나가 쪽 전체를 덮는다 (#12)
TEST(TileSchedulerPoster, PreviewCoversWholePosterPage) {
  const std::vector<engine::PageSize> pages{{10000, 7000}, {612, 792}};
  PageLayout layout(pages, {12, 16});
  layout.set_scale_q(8);
  SchedulerOptions options;
  options.preview_scale_q = 8;
  const auto needed = detail::compute_needed(layout, {0, 0, 400, 400}, options);
  bool found = false;
  for (const auto& n : needed) {
    if (n.key.page == 0 && n.key.tx == 0 && n.key.ty == 0 && n.key.scale_q < 8) {
      found = true;
      EXPECT_EQ(n.key.scale_q, preview_scale_q_for(pages[0], 8));
      const Size px = page_pixel_size(pages[0], n.key.scale_q);
      EXPECT_LE(px.w, kTileSize);
      EXPECT_LE(px.h, kTileSize);
    }
  }
  EXPECT_TRUE(found) << "poster page preview must use a smaller scale";
}

}  // namespace
}  // namespace onpdf::view
