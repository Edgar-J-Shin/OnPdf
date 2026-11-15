#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/edit/rotate_page.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

namespace onpdf::edit {
namespace {

using engine::PageSize;
using onpdf::test::testdata;

// 회전하면 가로·세로가 바뀌는 가짜 문서
class FakeDocument final : public engine::EditableDocument {
 public:
  explicit FakeDocument(std::vector<PageSize> sizes) : base_(std::move(sizes)), rotation_(base_.size(), 0) {}
  [[nodiscard]] int page_count() const noexcept override { return static_cast<int>(base_.size()); }
  [[nodiscard]] Result<PageSize> page_size(int page) const override {
    if (!valid(page)) return make_error(ErrorCode::kPageNotFound, "page");
    const PageSize b = base_[static_cast<std::size_t>(page)];
    return rotation_[static_cast<std::size_t>(page)] % 2 ? PageSize{b.height, b.width} : b;
  }
  [[nodiscard]] Result<int> page_rotation(int page) const override {
    if (!valid(page)) return make_error(ErrorCode::kPageNotFound, "page");
    return rotation_[static_cast<std::size_t>(page)];
  }
  [[nodiscard]] Result<void> set_page_rotation(int page, int q) override {
    if (!valid(page)) return make_error(ErrorCode::kPageNotFound, "page");
    rotation_[static_cast<std::size_t>(page)] = q;
    return {};
  }

 private:
  [[nodiscard]] bool valid(int page) const { return page >= 0 && page < page_count(); }
  std::vector<PageSize> base_;
  std::vector<int> rotation_;
};

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
