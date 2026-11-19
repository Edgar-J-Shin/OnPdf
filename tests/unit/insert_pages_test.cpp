#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/edit/insert_pages.h"

#include "../fake_document.h"
#include "../test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace onpdf::edit {
namespace {

using engine::PageSize;
using onpdf::test::FakeDocument;
using onpdf::test::FakeFile;
using onpdf::test::FakePage;
using onpdf::test::testdata;

TEST(InsertPagesCommand, ExecuteUndoRedoOnFake) {
  FakeDocument doc({{1, 1}, {2, 2}});
  doc.add_file("src.pdf", FakeFile{{{{9, 9}, 0, 100}, {{8, 8}, 0, 101}, {{7, 7}, 0, 102}}, "", true});
  InsertPagesCommand cmd("src.pdf", "", {2, 0}, 1);  // 소스 0·2 쪽을 1번 자리에
  auto change = cmd.execute(doc);
  ASSERT_TRUE(change) << change.error().message;
  EXPECT_TRUE(change->structural);
  EXPECT_EQ(doc.labels(), (std::vector<int>{0, 100, 102, 1}));
  EXPECT_EQ(cmd.inserted_count(), 2);
  EXPECT_EQ(change->selected_pages, (std::vector<int>{1, 2}));
  EXPECT_EQ(change->focus_page, 1);
  EXPECT_EQ(doc.snapshot_count(), 1);
  ASSERT_TRUE(cmd.undo(doc));
  EXPECT_EQ(doc.labels(), (std::vector<int>{0, 1}));
  EXPECT_EQ(doc.snapshot_count(), 1);  // 소스는 다시 실행을 위해 유지
  ASSERT_TRUE(cmd.execute(doc));
  EXPECT_EQ(doc.labels(), (std::vector<int>{0, 100, 102, 1}));
  cmd.discard(doc);
  EXPECT_EQ(doc.snapshot_count(), 0);
}

TEST(InsertPagesCommand, WholeSourceAtEndAndErrors) {
  FakeDocument doc({{1, 1}});
  doc.add_file("src.pdf", FakeFile{{{{9, 9}, 0, 100}, {{8, 8}, 0, 101}}, "", true});
  doc.add_file("locked.pdf", FakeFile{{{{9, 9}, 0, 200}}, "pw", true});
  doc.add_file("noextract.pdf", FakeFile{{{{9, 9}, 0, 300}}, "", false});
  InsertPagesCommand all("src.pdf", "", {}, 1);
  ASSERT_TRUE(all.execute(doc));
  EXPECT_EQ(doc.labels(), (std::vector<int>{0, 100, 101}));
  ASSERT_TRUE(all.undo(doc));
  EXPECT_EQ(doc.labels(), (std::vector<int>{0}));

  InsertPagesCommand missing("nope.pdf", "", {}, 0);
  auto r1 = missing.execute(doc);
  ASSERT_FALSE(r1);
  EXPECT_EQ(r1.error().code, ErrorCode::kFileNotFound);
  InsertPagesCommand wrong_pw("locked.pdf", "", {}, 0);
  auto r2 = wrong_pw.execute(doc);
  ASSERT_FALSE(r2);
  EXPECT_EQ(r2.error().code, ErrorCode::kPasswordRequired);
  InsertPagesCommand right_pw("locked.pdf", "pw", {}, 0);
  EXPECT_TRUE(right_pw.execute(doc));
  ASSERT_TRUE(right_pw.undo(doc));
  const int held = doc.snapshot_count();  // right_pw 가 재실행용으로 쥔 소스
  InsertPagesCommand forbidden("noextract.pdf", "", {}, 0);
  auto r3 = forbidden.execute(doc);
  ASSERT_FALSE(r3);
  EXPECT_EQ(r3.error().code, ErrorCode::kPermissionDenied);
  EXPECT_EQ(doc.snapshot_count(), held);  // 실패한 실행은 열어 둔 소스를 버린다 (리뷰 W2)
  InsertPagesCommand bad_page("src.pdf", "", {5}, 0);
  EXPECT_FALSE(bad_page.execute(doc));
  InsertPagesCommand bad_at("src.pdf", "", {}, 9);
  EXPECT_FALSE(bad_at.execute(doc));
  EXPECT_EQ(doc.snapshot_count(), held);
  EXPECT_FALSE(bad_at.undo(doc));  // 실행 전 undo
  EXPECT_EQ(doc.labels(), (std::vector<int>{0}));
}

// 한글 문서의 쪽을 가져온다: 실행 → 취소 → 재실행 (CLAUDE.md), 가져온 쪽의 텍스트가 한글
TEST(InsertPagesCommand, ExecuteUndoRedoOnPdfium) {
  auto doc = engine::PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  const PageSize p0 = *doc->page_size(0);
  const PageSize p1 = *doc->page_size(1);
  InsertPagesCommand cmd(testdata("korean-embedded.pdf"), "", {}, 1);
  auto change = cmd.execute(*doc);
  ASSERT_TRUE(change) << change.error().message;
  ASSERT_EQ(doc->page_count(), 3);
  EXPECT_EQ(cmd.inserted_count(), 1);
  EXPECT_EQ(change->all_sizes.size(), 3u);
  auto text = doc->load_text(1);
  ASSERT_TRUE(text) << text.error().message;
  const std::string utf8 = text->all_text();
  bool non_ascii = false;
  for (char ch : utf8) non_ascii = non_ascii || static_cast<unsigned char>(ch) >= 0x80;
  EXPECT_TRUE(non_ascii) << "inserted page should carry Korean text: " << utf8;
  EXPECT_DOUBLE_EQ(doc->page_size(0)->width, p0.width);
  EXPECT_DOUBLE_EQ(doc->page_size(2)->width, p1.width);

  ASSERT_TRUE(cmd.undo(*doc));
  ASSERT_EQ(doc->page_count(), 2);
  EXPECT_DOUBLE_EQ(doc->page_size(1)->width, p1.width);
  EXPECT_EQ(doc->snapshot_count(), 1);
  ASSERT_TRUE(cmd.execute(*doc));
  EXPECT_EQ(doc->page_count(), 3);
  cmd.discard(*doc);
  EXPECT_EQ(doc->snapshot_count(), 0);
}

TEST(InsertPagesCommand, PdfiumEncryptedSourceNeedsPassword) {
  auto doc = engine::PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  InsertPagesCommand no_pw(testdata("encrypted-aes256.pdf"), "", {}, 2);
  auto r = no_pw.execute(*doc);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, ErrorCode::kPasswordRequired);
  EXPECT_EQ(doc->page_count(), 2);
  InsertPagesCommand with_pw(testdata("encrypted-aes256.pdf"), "user", {0}, 2);
  auto ok = with_pw.execute(*doc);
  ASSERT_TRUE(ok) << ok.error().message;
  EXPECT_EQ(doc->page_count(), 3);
  ASSERT_TRUE(with_pw.undo(*doc));
  EXPECT_EQ(doc->page_count(), 2);
}

TEST(InsertPagesCommand, PdfiumSourceForbiddingExtractionIsRefused) {
  auto doc = engine::PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  InsertPagesCommand cmd(testdata("perm-no-extract.pdf"), "", {}, 0);
  auto r = cmd.execute(*doc);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, ErrorCode::kPermissionDenied);
  EXPECT_EQ(doc->page_count(), 2);
  // 조립만 금지된 문서는 소스로 쓸 수 있다 (추출 비트 5 는 허용)
  InsertPagesCommand ok(testdata("perm-no-assemble.pdf"), "", {0}, 0);
  EXPECT_TRUE(ok.execute(*doc));
  EXPECT_EQ(doc->page_count(), 3);
}

}  // namespace
}  // namespace onpdf::edit
