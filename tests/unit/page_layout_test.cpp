#include "onpdf/core/view/page_layout.h"
#include "onpdf/core/view/tile.h"

#include <gtest/gtest.h>

namespace onpdf::view {
namespace {

// mixed-sizes.pdf 와 같은 7쪽 (A4, A4 가로, Letter, A5, 명함, A4 회전(가로), Letter 회전(가로))
std::vector<engine::PageSize> mixed() {
  return {{595.276, 841.89}, {841.89, 595.276}, {612, 792}, {419.528, 595.276},
          {252, 144},        {841.89, 595.276}, {792, 612}};
}

TEST(PageLayout, EmptyLayout) {
  PageLayout layout({});
  EXPECT_EQ(layout.page_count(), 0);
  EXPECT_DOUBLE_EQ(layout.total_height(), 0.0);
  EXPECT_EQ(layout.page_at(10), -1);
  EXPECT_TRUE(layout.pages_in({0, 0, 100, 100}).empty());
}

TEST(PageLayout, StacksPagesVerticallyWithGapsAndCentering) {
  PageLayout layout(mixed(), {12, 16});
  layout.set_scale_q(64);  // 1 px/pt
  const Rect p0 = layout.page_rect(0);
  const Rect p1 = layout.page_rect(1);
  EXPECT_DOUBLE_EQ(p0.y, 16);
  EXPECT_DOUBLE_EQ(p0.h, 842);  // ceil(841.89)
  EXPECT_DOUBLE_EQ(p1.y, 16 + 842 + 12);
  // 가장 넓은 쪽(842px) 기준 가운데 정렬
  EXPECT_DOUBLE_EQ(layout.document_width(), 842 + 32);
  EXPECT_DOUBLE_EQ(p1.x, 16);
  EXPECT_DOUBLE_EQ(p0.x, 16 + (842 - 596) / 2.0);
  const Rect card = layout.page_rect(4);
  EXPECT_DOUBLE_EQ(card.x, 16 + (842 - 252) / 2.0);
  EXPECT_DOUBLE_EQ(layout.total_height(), layout.page_rect(6).bottom() + 16);
}

TEST(PageLayout, PageAtHandlesGapsAndBounds) {
  PageLayout layout(mixed(), {12, 16});
  layout.set_scale_q(64);
  EXPECT_EQ(layout.page_at(-100), 0);
  EXPECT_EQ(layout.page_at(0), 0);
  EXPECT_EQ(layout.page_at(16 + 842), 0);      // 간격은 위쪽 쪽에 귀속
  EXPECT_EQ(layout.page_at(16 + 842 + 12), 1);  // 다음 쪽 시작
  EXPECT_EQ(layout.page_at(1e9), 6);
}

TEST(PageLayout, PagesInViewport) {
  PageLayout layout(mixed(), {12, 16});
  layout.set_scale_q(64);
  EXPECT_EQ(layout.pages_in({0, 0, 900, 100}), (std::vector<int>{0}));
  EXPECT_EQ(layout.pages_in({0, 800, 900, 100}), (std::vector<int>{0, 1}));
  // 간격 안에만 걸치면 어느 쪽도 포함하지 않는다
  EXPECT_TRUE(layout.pages_in({0, 16 + 842 + 1, 900, 5}).empty());
  EXPECT_TRUE(layout.pages_in({0, 0, 900, 0}).empty());
}

TEST(PageLayout, CoordinateRoundTrip) {
  PageLayout layout(mixed(), {12, 16});
  layout.set_scale_q(128);  // 2 px/pt
  const Point doc{100, 1000};
  const int page = layout.page_at(doc.y);
  const Point in_page = layout.doc_to_page_px(page, doc);
  const Point back = layout.page_px_to_doc(page, in_page);
  EXPECT_DOUBLE_EQ(back.x, doc.x);
  EXPECT_DOUBLE_EQ(back.y, doc.y);
  const Point pt = layout.page_px_to_pt(page, {200, 400});
  EXPECT_DOUBLE_EQ(pt.x, 100);
  EXPECT_DOUBLE_EQ(pt.y, 200);
}

TEST(PageLayout, FitScales) {
  PageLayout layout(mixed(), {12, 16});
  // 뷰포트 875px - 32 여백 - 1 여유 = 842 → 가장 넓은 쪽 841.89pt 에 1.0 배율 (내림)
  EXPECT_EQ(layout.fit_width_scale_q(875), 64);
  EXPECT_EQ(layout.fit_width_scale_q(874), 63);  // 841/841.89 < 1.0 → 내림
  EXPECT_EQ(layout.fit_width_scale_q(437 + 33), 33);  // 437/841.89 ≈ 0.519 → floor(33.2)
  // Letter(612×792) 를 400×400 에 맞추면 높이 기준은 (400-33)/792 ≈ 0.4634 이지만, 가장 넓은 쪽(841.89pt) 도
  // 뷰 너비에 들어가야 하므로 (400-33)/841.89 ≈ 0.4359 → floor(27.9) = 27 로 상한된다 (가로 스크롤바 진동 방지)
  EXPECT_EQ(layout.fit_page_scale_q(400, 400, 2), 27);
  // 높이 제한이 더 빡빡한 경우: 세로 200px → (200-33)/792 ≈ 0.2109 → 13
  EXPECT_EQ(layout.fit_page_scale_q(2000, 200, 2), 13);
  // 맞춤 결과로 배치한 문서는 뷰포트보다 넓지 않다
  layout.set_scale_q(layout.fit_width_scale_q(700));
  EXPECT_LE(layout.document_width(), 700.0);
  // 배율을 바꾸면 레이아웃이 다시 계산된다
  layout.set_scale_q(32);
  EXPECT_DOUBLE_EQ(layout.page_rect(0).h, 421);  // ceil(841.89 * 0.5)
}

}  // namespace
}  // namespace onpdf::view
