#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/edit/delete_pages.h"

#include "../fake_document.h"
#include "../test_support.h"

#include <gtest/gtest.h>

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

bool same_sizes(const std::vector<PageSize>& a, const std::vector<PageSize>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::abs(a[i].width - b[i].width) > 0.01 || std::abs(a[i].height - b[i].height) > 0.01) return false;
  }
  return true;
}

// CLAUDE.md: 실행 → 취소 → 재실행 후 문서 상태 비교
TEST(DeletePagesCommand, ExecuteUndoRedoOnFake) {
  FakeDocument doc({{1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}});
  ASSERT_TRUE(doc.set_page_rotation(3, 1));  // 회전된 쪽도 복원되어야 한다
  DeletePagesCommand cmd({3, 1});            // 비연속, 정렬 전
  auto change = cmd.execute(doc);
  ASSERT_TRUE(change) << change.error().message;
  EXPECT_TRUE(change->structural);
  EXPECT_EQ(change->all_sizes.size(), 3u);
  EXPECT_EQ(doc.labels(), (std::vector<int>{0, 2, 4}));
  EXPECT_EQ(doc.snapshot_count(), 1);

  ASSERT_TRUE(cmd.undo(doc));
  EXPECT_EQ(doc.labels(), (std::vector<int>{0, 1, 2, 3, 4}));
  EXPECT_EQ(doc.rotations(), (std::vector<int>{0, 0, 0, 1, 0}));
  EXPECT_DOUBLE_EQ(doc.page_size(3)->width, 4);  // 회전된 사본: 폭=높이 (4,4 라 같다) — 라벨로 정체성 확인

  ASSERT_TRUE(cmd.execute(doc));  // 다시 실행: 스냅샷을 새로 뜬다 (이전 것은 해제)
  EXPECT_EQ(doc.labels(), (std::vector<int>{0, 2, 4}));
  EXPECT_EQ(doc.snapshot_count(), 1);
  cmd.discard(doc);
  EXPECT_EQ(doc.snapshot_count(), 0);
}

TEST(DeletePagesCommand, RefusesAllAndInvalidWithoutChange) {
  FakeDocument doc({{1, 1}, {2, 2}});
  DeletePagesCommand all({0, 1});
  EXPECT_FALSE(all.execute(doc));
  EXPECT_EQ(doc.page_count(), 2);
  DeletePagesCommand bad({5});
  EXPECT_FALSE(bad.execute(doc));
  EXPECT_FALSE(bad.undo(doc));  // 실행 전 undo
  EXPECT_EQ(doc.labels(), (std::vector<int>{0, 1}));
  EXPECT_EQ(doc.snapshot_count(), 0);
  DeletePagesCommand none({});
  EXPECT_FALSE(none.execute(doc));
}

TEST(DeletePagesCommand, ExecuteUndoRedoOnPdfium) {
  auto doc = engine::PdfiumDocument::open(testdata("mixed-sizes.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  ASSERT_EQ(doc->page_count(), 7);
  ASSERT_TRUE(doc->set_page_rotation(2, 1));
  const auto before = sizes_of(*doc);
  DeletePagesCommand cmd({2, 5});
  auto change = cmd.execute(*doc);
  ASSERT_TRUE(change) << change.error().message;
  ASSERT_EQ(doc->page_count(), 5);
  EXPECT_TRUE(change->structural);
  EXPECT_TRUE(same_sizes(change->all_sizes, sizes_of(*doc)));
  // 남은 쪽은 원래의 0,1,3,4,6
  const std::vector<PageSize> expected{before[0], before[1], before[3], before[4], before[6]};
  EXPECT_TRUE(same_sizes(sizes_of(*doc), expected));
  EXPECT_EQ(doc->snapshot_count(), 1);

  auto undone = cmd.undo(*doc);
  ASSERT_TRUE(undone) << undone.error().message;
  ASSERT_EQ(doc->page_count(), 7);
  EXPECT_TRUE(same_sizes(sizes_of(*doc), before));  // 회전된 2쪽 크기(가로)도 그대로
  EXPECT_EQ(*doc->page_rotation(2), 1);

  ASSERT_TRUE(cmd.execute(*doc));
  EXPECT_EQ(doc->page_count(), 5);
  EXPECT_TRUE(same_sizes(sizes_of(*doc), expected));
  EXPECT_EQ(doc->snapshot_count(), 1);
  cmd.discard(*doc);
  EXPECT_EQ(doc->snapshot_count(), 0);
}

TEST(DeletePagesCommand, PdfiumRefusesDeletingEveryPage) {
  auto doc = engine::PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  DeletePagesCommand cmd({0, 1});
  auto r = cmd.execute(*doc);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, ErrorCode::kInvalidArgument);
  EXPECT_EQ(doc->page_count(), 2);
  EXPECT_EQ(doc->snapshot_count(), 0);
}

}  // namespace
}  // namespace onpdf::edit
