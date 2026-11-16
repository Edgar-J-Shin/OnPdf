#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/edit/move_pages.h"

#include "../fake_document.h"
#include "../test_support.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace onpdf::edit {
namespace {

using engine::PageSize;
using onpdf::test::FakeDocument;
using onpdf::test::testdata;

std::vector<PageSize> sizes_of(const engine::EditableDocument& doc) {
  std::vector<PageSize> out;
  for (int i = 0; i < doc.page_count(); ++i) out.push_back(*doc.page_size(i));
  return out;
}

std::vector<PageSize> reorder(const std::vector<PageSize>& sizes, const std::vector<int>& order) {
  std::vector<PageSize> out;
  for (int old : order) out.push_back(sizes[static_cast<std::size_t>(old)]);
  return out;
}

bool same_sizes(const std::vector<PageSize>& a, const std::vector<PageSize>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::abs(a[i].width - b[i].width) > 0.01 || std::abs(a[i].height - b[i].height) > 0.01) return false;
  }
  return true;
}

TEST(MovePagesCommand, ExecuteUndoRedoOnFake) {
  FakeDocument doc({{1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}});
  MovePagesCommand cmd({4, 1}, 0);  // 비연속 블록 [1,4] 를 맨 앞으로
  auto change = cmd.execute(doc);
  ASSERT_TRUE(change) << change.error().message;
  EXPECT_TRUE(change->structural);
  EXPECT_EQ(doc.labels(), (std::vector<int>{1, 4, 0, 2, 3}));
  ASSERT_TRUE(cmd.undo(doc));
  EXPECT_EQ(doc.labels(), (std::vector<int>{0, 1, 2, 3, 4}));
  ASSERT_TRUE(cmd.execute(doc));
  EXPECT_EQ(doc.labels(), (std::vector<int>{1, 4, 0, 2, 3}));
}

TEST(MovePagesCommand, BlockToEndAndBack) {
  FakeDocument doc({{1, 1}, {2, 2}, {3, 3}, {4, 4}});
  MovePagesCommand cmd({0, 1}, 2);  // 앞 두 쪽을 끝으로 (dest = count − n)
  ASSERT_TRUE(cmd.execute(doc));
  EXPECT_EQ(doc.labels(), (std::vector<int>{2, 3, 0, 1}));
  ASSERT_TRUE(cmd.undo(doc));
  EXPECT_EQ(doc.labels(), (std::vector<int>{0, 1, 2, 3}));
}

TEST(MovePagesCommand, InvalidArgumentsLeaveDocumentUnchanged) {
  FakeDocument doc({{1, 1}, {2, 2}, {3, 3}});
  MovePagesCommand too_far({0}, 3);
  EXPECT_FALSE(too_far.execute(doc));
  EXPECT_FALSE(too_far.undo(doc));  // 실행 전 undo
  MovePagesCommand out_of_range({7}, 0);
  EXPECT_FALSE(out_of_range.execute(doc));
  MovePagesCommand empty({}, 0);
  EXPECT_FALSE(empty.execute(doc));
  EXPECT_EQ(doc.labels(), (std::vector<int>{0, 1, 2}));
  EXPECT_EQ(doc.move_calls, 0);
}

// CLAUDE.md: 실행 → 취소 → 재실행 후 문서 상태 비교 (쪽 크기 열로 정체성 확인)
TEST(MovePagesCommand, ExecuteUndoRedoOnPdfium) {
  auto doc = engine::PdfiumDocument::open(testdata("mixed-sizes.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  ASSERT_EQ(doc->page_count(), 7);
  const auto before = sizes_of(*doc);
  MovePagesCommand cmd({6, 2}, 1);  // 결과: 0, 2, 6, 1, 3, 4, 5
  auto change = cmd.execute(*doc);
  ASSERT_TRUE(change) << change.error().message;
  EXPECT_EQ(doc->page_count(), 7);
  const std::vector<int> expected_order{0, 2, 6, 1, 3, 4, 5};
  EXPECT_TRUE(same_sizes(sizes_of(*doc), reorder(before, expected_order)));
  EXPECT_TRUE(same_sizes(change->all_sizes, sizes_of(*doc)));

  auto undone = cmd.undo(*doc);
  ASSERT_TRUE(undone) << undone.error().message;
  EXPECT_TRUE(same_sizes(sizes_of(*doc), before));

  ASSERT_TRUE(cmd.execute(*doc));
  EXPECT_TRUE(same_sizes(sizes_of(*doc), reorder(before, expected_order)));
}

TEST(MovePagesCommand, PdfiumRejectsBadDestinationWithoutChange) {
  auto doc = engine::PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  const auto before = sizes_of(*doc);
  MovePagesCommand cmd({0, 1}, 1);  // dest > count − n
  auto r = cmd.execute(*doc);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, ErrorCode::kInvalidArgument);
  EXPECT_TRUE(same_sizes(sizes_of(*doc), before));
}

}  // namespace
}  // namespace onpdf::edit
