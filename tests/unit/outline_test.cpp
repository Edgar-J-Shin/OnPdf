#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/view/outline.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

namespace onpdf::view {
namespace {

using engine::PdfiumDocument;
using onpdf::test::testdata;

const OutlineItem& at(const Outline& outline, int index) {
  return outline.items.at(static_cast<std::size_t>(index));
}

OutlineItem page_item(int page, double y) {
  OutlineItem item;
  Link link;
  link.kind = LinkKind::kPage;
  link.page = page;
  item.target = link;
  item.approx_y = y;
  return item;
}

TEST(Outline, ItemAtFindsLastItemAtOrBeforePosition) {
  Outline outline;
  outline.items.push_back(page_item(0, 0));    // 0
  outline.items.push_back(page_item(2, 100));  // 1
  outline.items.push_back(page_item(2, 100));  // 2 (같은 위치, 뒤 항목)
  outline.items.push_back(OutlineItem{});      // 3 대상 없음 → 색인 제외
  outline.items.push_back(page_item(1, 500));  // 4 (문서 순서가 뒤바뀐 항목)
  outline.build_position_index();

  EXPECT_EQ(outline.item_at(0, 0), 0);
  EXPECT_EQ(outline.item_at(0, 700), 0);
  EXPECT_EQ(outline.item_at(1, 499), 0);
  EXPECT_EQ(outline.item_at(1, 500), 4);
  EXPECT_EQ(outline.item_at(2, 99), 4);
  EXPECT_EQ(outline.item_at(2, 100), 2);
  EXPECT_EQ(outline.item_at(9, 0), 2);
}

TEST(Outline, ItemAtBeforeFirstTargetIsNone) {
  Outline outline;
  outline.items.push_back(page_item(3, 50));
  outline.build_position_index();
  EXPECT_EQ(outline.item_at(0, 0), -1);
  EXPECT_EQ(outline.item_at(3, 49), -1);
  EXPECT_EQ(outline.item_at(3, 50), 0);

  Outline empty;
  empty.build_position_index();
  EXPECT_EQ(empty.item_at(0, 0), -1);
}

TEST(OutlineLoad, KoreanThreeLevelBookmarks) {
  auto doc = PdfiumDocument::open(testdata("bookmarks.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  auto outline = doc->load_outline();
  ASSERT_TRUE(outline) << outline.error().message;

  ASSERT_EQ(outline->size(), 15);
  ASSERT_EQ(outline->roots.size(), 3u);
  EXPECT_FALSE(outline->truncated);
  EXPECT_FALSE(outline->cycle_detected);

  const auto& ch1 = at(*outline, 0);
  EXPECT_EQ(ch1.title, "제 1 장 (page 1)");
  EXPECT_EQ(ch1.depth, 0);
  EXPECT_EQ(ch1.parent, -1);
  EXPECT_TRUE(ch1.open);
  ASSERT_EQ(ch1.children.size(), 2u);
  ASSERT_TRUE(ch1.target);
  EXPECT_EQ(ch1.target->kind, LinkKind::kPage);
  EXPECT_EQ(ch1.target->page, 0);
  ASSERT_TRUE(ch1.target->user_location);
  EXPECT_DOUBLE_EQ(ch1.target->user_location->y, 792.0);
  EXPECT_NEAR(ch1.approx_y, 0.0, 1e-6);

  const auto& s11 = at(*outline, ch1.children[0]);
  EXPECT_EQ(s11.title, "1.1 절 (page 2)");
  EXPECT_EQ(s11.depth, 1);
  EXPECT_EQ(s11.parent, 0);
  ASSERT_TRUE(s11.target);
  EXPECT_EQ(s11.target->page, 1);

  const auto& s111 = at(*outline, s11.children.at(0));
  EXPECT_EQ(s111.title, "1.1.1 항 (page 3)");
  EXPECT_EQ(s111.depth, 2);
  EXPECT_FALSE(s111.open);
  ASSERT_TRUE(s111.target);
  EXPECT_EQ(s111.target->page, 2);

  for (int root : outline->roots) EXPECT_TRUE(at(*outline, root).open);
  EXPECT_EQ(at(*outline, outline->roots[1]).title, "제 2 장 (page 4)");

  // 현재 항목: 쪽 맨 위가 대상이므로 쪽 번호로 결정된다
  EXPECT_EQ(outline->item_at(0, 300), 0);
  EXPECT_EQ(at(*outline, outline->item_at(1, 10)).title, "1.1 절 (page 2)");
  EXPECT_EQ(at(*outline, outline->item_at(9, 700)).title, "3.2.1 항 (page 10)");
}

TEST(OutlineLoad, PdfiumSampleHasUriAndEmptyTargets) {
  auto doc = PdfiumDocument::open(testdata("pdfium-bookmarks.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  auto outline = doc->load_outline();
  ASSERT_TRUE(outline) << outline.error().message;
  ASSERT_FALSE(outline->empty());
  bool has_uri = false;
  bool has_none = false;
  for (const auto& item : outline->items) {
    if (!item.target) has_none = true;
    else if (item.target->kind == LinkKind::kUri) has_uri = true;
  }
  EXPECT_TRUE(has_uri);
  EXPECT_TRUE(has_none);
}

TEST(OutlineLoad, DocumentWithoutOutlineIsEmpty) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  auto outline = doc->load_outline();
  ASSERT_TRUE(outline) << outline.error().message;
  EXPECT_TRUE(outline->empty());
  EXPECT_TRUE(outline->roots.empty());
  EXPECT_EQ(outline->item_at(0, 0), -1);
}

TEST(OutlineLoad, MaliciousCyclesAndDepthTerminate) {
  auto doc = PdfiumDocument::open(testdata("outline-cycle.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  auto outline = doc->load_outline();
  ASSERT_TRUE(outline) << outline.error().message;
  EXPECT_TRUE(outline->cycle_detected);
  EXPECT_TRUE(outline->truncated);
  ASSERT_EQ(outline->roots.size(), 2u);
  EXPECT_EQ(at(*outline, outline->roots[0]).title, "A self child");
  EXPECT_EQ(at(*outline, outline->roots[1]).title, "B sibling cycle");
  EXPECT_LE(outline->size(), 2 + Outline::kMaxDepth);
  for (const auto& item : outline->items) {
    EXPECT_LT(item.depth, Outline::kMaxDepth);
    for (int child : item.children) {
      ASSERT_GE(child, 0);
      ASSERT_LT(child, outline->size());
      EXPECT_EQ(at(*outline, child).depth, item.depth + 1);
    }
  }
}

TEST(OutlineLoad, ResolveLocationMapsUserSpaceToPagePoints) {
  auto doc = PdfiumDocument::open(testdata("bookmarks.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  auto top = doc->resolve_location(0, Point{0, 792});
  ASSERT_TRUE(top) << top.error().message;
  EXPECT_NEAR(top->x, 0.0, 0.01);
  EXPECT_NEAR(top->y, 0.0, 0.01);
  auto mid = doc->resolve_location(3, Point{100, 692});
  ASSERT_TRUE(mid) << mid.error().message;
  EXPECT_NEAR(mid->x, 100.0, 0.01);
  EXPECT_NEAR(mid->y, 100.0, 0.01);

  EXPECT_FALSE(doc->resolve_location(10, Point{0, 0}));
  EXPECT_FALSE(doc->resolve_location(-1, Point{0, 0}));
}

}  // namespace
}  // namespace onpdf::view
