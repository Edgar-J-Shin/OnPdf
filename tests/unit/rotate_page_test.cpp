#include "onpdf/edit/rotate_page.h"

#include "onpdf/core/engine/pdfium_document.h"

#include "../fake_document.h"
#include "../test_support.h"
#include <gtest/gtest.h>
#include <utility>
#include <vector>

namespace onpdf::edit {
namespace {

using engine::PageSize;
using onpdf::test::FakeDocument;
using onpdf::test::testdata;

TEST(RotatePageCommand, ExecuteUndoRedoOnFake) {
  FakeDocument doc({{612, 792}, {612, 792}});
  RotatePageCommand cw(1, 1);
  auto change = cw.execute(doc);
  ASSERT_TRUE(change) << change.error().message;
  EXPECT_EQ(change->pages, std::vector<int>{1});
  ASSERT_EQ(change->new_sizes.size(), 1u);
  EXPECT_DOUBLE_EQ(change->new_sizes[0].second.width, 792);
  EXPECT_EQ(*doc.page_rotation(1), 1);
  EXPECT_EQ(*doc.page_rotation(0), 0);  // 다른 쪽은 그대로

  ASSERT_TRUE(cw.undo(doc));
  EXPECT_EQ(*doc.page_rotation(1), 0);
  ASSERT_TRUE(cw.execute(doc));  // 다시 실행
  EXPECT_EQ(*doc.page_rotation(1), 1);

  RotatePageCommand ccw(1, -1);  // 반시계 = 3
  ASSERT_TRUE(ccw.execute(doc));
  EXPECT_EQ(*doc.page_rotation(1), 0);
  RotatePageCommand four(1, 3);
  ASSERT_TRUE(four.execute(doc));
  EXPECT_EQ(*doc.page_rotation(1), 3);  // 0 + 3
}

// 여러 쪽을 한 실행 취소 단위로 돌린다
TEST(RotatePageCommand, MultiplePagesExecuteUndoRedo) {
  FakeDocument doc({{612, 792}, {612, 792}, {612, 792}});
  ASSERT_TRUE(doc.set_page_rotation(2, 3));
  RotatePageCommand cmd({2, 0, 2}, 1);  // 정렬·중복 제거 → {0, 2}
  EXPECT_EQ(cmd.pages(), (std::vector<int>{0, 2}));
  auto change = cmd.execute(doc);
  ASSERT_TRUE(change) << change.error().message;
  EXPECT_EQ(change->pages, (std::vector<int>{0, 2}));
  EXPECT_EQ(change->new_sizes.size(), 2u);
  EXPECT_EQ(doc.rotations(), (std::vector<int>{1, 0, 0}));  // 2쪽: 3 + 1 = 0
  ASSERT_TRUE(cmd.undo(doc));
  EXPECT_EQ(doc.rotations(), (std::vector<int>{0, 0, 3}));
  ASSERT_TRUE(cmd.execute(doc));
  EXPECT_EQ(doc.rotations(), (std::vector<int>{1, 0, 0}));
  RotatePageCommand bad({0, 7}, 1);  // 한 쪽이 잘못되면 아무것도 바꾸지 않는다
  EXPECT_FALSE(bad.execute(doc));
  EXPECT_EQ(doc.rotations(), (std::vector<int>{1, 0, 0}));
}

TEST(RotatePageCommand, InvalidPageFailsWithoutChange) {
  FakeDocument doc({{612, 792}});
  RotatePageCommand bad(5, 1);
  EXPECT_FALSE(bad.execute(doc));
  EXPECT_FALSE(bad.undo(doc));  // 실행 전 undo
  EXPECT_EQ(*doc.page_rotation(0), 0);
}

// CLAUDE.md: 모든 Command 는 실행 → 취소 → 재실행 후 문서 상태 비교
TEST(RotatePageCommand, ExecuteUndoRedoOnPdfium) {
  auto doc = engine::PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  const PageSize before = *doc->page_size(0);
  RotatePageCommand cmd(0, 1);

  auto done = cmd.execute(*doc);
  ASSERT_TRUE(done) << done.error().message;
  EXPECT_EQ(*doc->page_rotation(0), 1);
  const PageSize rotated = *doc->page_size(0);
  EXPECT_DOUBLE_EQ(rotated.width, before.height);
  EXPECT_DOUBLE_EQ(rotated.height, before.width);
  EXPECT_DOUBLE_EQ(done->new_sizes[0].second.width, rotated.width);

  ASSERT_TRUE(cmd.undo(*doc));
  EXPECT_EQ(*doc->page_rotation(0), 0);
  EXPECT_DOUBLE_EQ(doc->page_size(0)->width, before.width);
  EXPECT_DOUBLE_EQ(doc->page_size(0)->height, before.height);

  ASSERT_TRUE(cmd.execute(*doc));
  EXPECT_EQ(*doc->page_rotation(0), 1);
  EXPECT_DOUBLE_EQ(doc->page_size(0)->width, rotated.width);
  // 2쪽(가로, 원래 /Rotate 0)은 영향 없음
  EXPECT_EQ(*doc->page_rotation(1), 0);
}

}  // namespace
}  // namespace onpdf::edit
