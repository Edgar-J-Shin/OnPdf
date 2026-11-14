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
  std::map<TileKey, Pending> pending;
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

}  // namespace
}  // namespace onpdf::view
