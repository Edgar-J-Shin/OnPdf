#include "onpdf/core/view/search.h"

#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <memory>
#include <vector>

namespace onpdf::view {
namespace {

PageTextIndex index_with_sources(const std::vector<int>& sources) {
  PageTextIndex idx;
  for (std::size_t i = 0; i < sources.size(); ++i) {
    TextChar c;
    c.code = U'a';
    c.source_index = sources[i];
    c.box = {static_cast<double>(i) * 10, 0, 10, 12};
    idx.chars.push_back(c);
  }
  idx.lines.push_back({{0, static_cast<int>(sources.size())}, {0, 0, sources.size() * 10.0, 12}});
  return idx;
}

TEST(Search, MapSourceRangeSkipsLineBreaks) {
  // PDFium 인덱스 3,4 는 줄바꿈(\r\n)이라 인덱스에 없다
  const auto idx = index_with_sources({0, 1, 2, 5, 6, 7});
  EXPECT_EQ(map_source_range(idx, {1, 2}), (TextRange{1, 3}));   // 1,2
  EXPECT_EQ(map_source_range(idx, {2, 4}), (TextRange{2, 4}));   // 2,(3,4),5 → 인덱스 2..3
  EXPECT_EQ(map_source_range(idx, {3, 2}), (TextRange{}));       // 줄바꿈만
  EXPECT_EQ(map_source_range(idx, {5, 3}), (TextRange{3, 6}));
  EXPECT_EQ(map_source_range(idx, {7, 5}), (TextRange{5, 6}));   // 끝을 넘어도 clamp
  EXPECT_EQ(map_source_range(idx, {0, 0}), (TextRange{}));
  EXPECT_EQ(map_source_range(PageTextIndex{}, {0, 3}), (TextRange{}));
}

TEST(Search, NormalizeQuery) {
  EXPECT_EQ(normalize_query("  page  "), "page");
  EXPECT_EQ(normalize_query("a\xC2\xA0" "b"), "a b");          // NBSP
  EXPECT_EQ(normalize_query("한\xE3\x80\x80글"), "한 글");        // 전각 공백
  EXPECT_EQ(normalize_query("\t\n "), "");
  EXPECT_EQ(normalize_query("x\ty"), "x y");
}

// 요청을 기록하고 테스트가 완료를 주입하는 가짜 실행기
class FakeExecutor {
 public:
  struct Pending {
    std::string query;
    SearchOptions options;
    SearchSession::PageCallback cb;
    std::shared_ptr<std::atomic<bool>> cancelled;
  };
  SearchSession::Executor executor() {
    return [this](int page, const std::string& query, SearchOptions options, SearchSession::PageCallback cb) {
      auto flag = std::make_shared<std::atomic<bool>>(false);
      pending[page] = Pending{query, options, std::move(cb), flag};
      order.push_back(page);
      return engine::TaskHandle::for_testing(flag);
    };
  }
  // page 를 hits 개 결과(각 범위 [i*10, i*10+3))와 함께 완료
  void complete(int page, int hits, int chars = 100) {
    auto p = std::move(pending.at(page));
    pending.erase(page);
    PageSearchResult r;
    r.page = page;
    r.char_count = chars;
    for (int i = 0; i < hits; ++i) r.ranges.push_back({i * 10, i * 10 + 3});
    if (hits > 0) r.index = std::make_shared<const PageTextIndex>();
    p.cb(std::move(r));
  }
  void fail(int page) {
    auto p = std::move(pending.at(page));
    pending.erase(page);
    p.cb(make_error(ErrorCode::kRenderFailed, "boom"));
  }
  bool cancelled(int page) const { return pending.at(page).cancelled->load(); }
  std::map<int, Pending> pending;
  std::vector<int> order;
};

TEST(SearchSession, PageOrderStartsAtCurrentAndLimitsInFlight) {
  FakeExecutor fake;
  SearchSession s(fake.executor(), 2);
  s.start("q", {}, 5, 2);
  EXPECT_EQ(fake.order, (std::vector<int>{2, 3}));  // 최대 2개
  EXPECT_EQ(s.in_flight(), 2);
  fake.complete(2, 0);
  EXPECT_EQ(fake.order, (std::vector<int>{2, 3, 4}));
  fake.complete(3, 0);
  fake.complete(4, 0);
  EXPECT_EQ(fake.order, (std::vector<int>{2, 3, 4, 0, 1}));  // 끝에서 처음으로 순환
  fake.complete(0, 0);
  fake.complete(1, 0);
  EXPECT_FALSE(s.searching());
  EXPECT_EQ(s.pages_done(), 5);
  EXPECT_EQ(s.total(), 0u);
}

TEST(SearchSession, CurrentIsFirstHitAfterStartPageEvenIfEarlierPageArrivesFirst) {
  FakeExecutor fake;
  SearchSession s(fake.executor(), 4);
  int current_events = 0;
  s.on_current_changed = [&] { ++current_events; };
  s.start("q", {}, 5, 2);
  current_events = 0;
  // 쪽 3 결과가 먼저 와도, 쪽 2 가 아직 진행 중이면 현재를 정하지 않는다
  fake.complete(3, 2);
  EXPECT_FALSE(s.current().has_value());
  fake.complete(2, 0);  // 2 는 결과 없음 → 이제 3 의 첫 결과가 현재
  ASSERT_TRUE(s.current().has_value());
  EXPECT_EQ(s.current()->page, 3);
  EXPECT_EQ(s.current()->range, (TextRange{0, 3}));
  EXPECT_EQ(current_events, 1);
  // 문서 순서 번호: 0,1,2 쪽 결과는 아직 없음 → 1
  EXPECT_EQ(s.current_ordinal(), 1u);
}

TEST(SearchSession, NextPreviousAndWrapOnlyAfterSearchEnds) {
  FakeExecutor fake;
  SearchSession s(fake.executor(), 5);
  s.start("q", {}, 3, 0);
  fake.complete(0, 1);
  fake.complete(1, 2);
  ASSERT_TRUE(s.current().has_value());
  EXPECT_EQ(s.current()->page, 0);
  EXPECT_TRUE(s.next());
  EXPECT_EQ(s.current()->page, 1);
  EXPECT_TRUE(s.next());
  EXPECT_EQ(s.current_ordinal(), 3u);
  // 쪽 2 가 아직 검색 중이라 끝에서 순환하지 않는다
  EXPECT_FALSE(s.next());
  fake.complete(2, 0);
  EXPECT_FALSE(s.searching());
  EXPECT_TRUE(s.next());
  EXPECT_TRUE(s.wrapped());
  EXPECT_EQ(s.current_ordinal(), 1u);
  EXPECT_TRUE(s.previous());
  EXPECT_TRUE(s.wrapped());
  EXPECT_EQ(s.current_ordinal(), 3u);
  EXPECT_TRUE(s.previous());
  EXPECT_FALSE(s.wrapped());
  EXPECT_EQ(s.current_ordinal(), 2u);
  EXPECT_EQ(s.total(), 3u);
  ASSERT_NE(s.hits_on(1), nullptr);
  EXPECT_EQ(s.hits_on(1)->size(), 2u);
  EXPECT_EQ(s.hits_on(2), nullptr);
  EXPECT_NE(s.index_of(1), nullptr);
}

TEST(SearchSession, RestartCancelsAndIgnoresLateResults) {
  FakeExecutor fake;
  SearchSession s(fake.executor(), 2);
  s.start("old", {}, 4, 0);
  auto stale = fake.pending.at(0);  // 콜백 사본 (늦게 도착시킬 것)
  s.start("new", {}, 4, 0);
  EXPECT_TRUE(stale.cancelled->load());
  EXPECT_EQ(fake.pending.at(0).query, "new");
  // 이전 검색의 결과가 늦게 와도 무시
  PageSearchResult late;
  late.page = 0;
  late.ranges.push_back({0, 3});
  late.char_count = 10;
  stale.cb(std::move(late));
  EXPECT_EQ(s.total(), 0u);
  EXPECT_EQ(s.pages_done(), 0);
}

TEST(SearchSession, ClearAndEmptyQuery) {
  FakeExecutor fake;
  SearchSession s(fake.executor(), 2);
  s.start("q", {}, 3, 0);
  fake.complete(0, 1);
  EXPECT_TRUE(s.active());
  s.start("   ", {}, 3, 0);  // 빈 쿼리 → clear
  EXPECT_FALSE(s.active());
  EXPECT_EQ(s.total(), 0u);
  EXPECT_FALSE(s.current().has_value());
  EXPECT_TRUE(fake.cancelled(1));
}

TEST(SearchSession, TruncatesAtMaxHits) {
  FakeExecutor fake;
  SearchSession s(fake.executor(), 1, 5);
  s.start("q", {}, 3, 0);
  fake.complete(0, 4);
  fake.complete(1, 4);  // 8 > 5 → 5 에서 자르고 중단
  EXPECT_TRUE(s.truncated());
  EXPECT_EQ(s.total(), 5u);
  EXPECT_FALSE(s.searching());
  EXPECT_TRUE(fake.pending.empty());
}

TEST(SearchSession, FailedPageCountsAsDoneAndNoTextDetection) {
  FakeExecutor fake;
  SearchSession s(fake.executor(), 3);
  s.start("q", {}, 3, 0);
  fake.fail(0);
  fake.complete(1, 0, 0);
  EXPECT_FALSE(s.no_text());  // 아직 검색 중
  fake.complete(2, 0, 0);
  EXPECT_EQ(s.pages_done(), 3);
  EXPECT_TRUE(s.no_text());
  EXPECT_FALSE(s.next());
}

TEST(SearchSession, LateCallbackAfterDestructionIsIgnored) {
  FakeExecutor fake;
  SearchSession::PageCallback cb;
  {
    SearchSession s(fake.executor(), 1);
    s.start("q", {}, 2, 0);
    cb = fake.pending.at(0).cb;
  }
  PageSearchResult r;
  r.char_count = 1;
  cb(std::move(r));  // 크래시 없음
  SUCCEED();
}

}  // namespace
}  // namespace onpdf::view
