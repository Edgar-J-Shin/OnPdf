#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/save.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace onpdf::engine {
namespace {

using onpdf::test::read_bytes;
using onpdf::test::ScopedTempDir;
using onpdf::test::testdata;

std::size_t files_in(const std::filesystem::path& dir) {
  std::size_t n = 0;
  for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(dir)) ++n;
  return n;
}

TEST(SaveDocument, RotatedPageSurvivesSaveAndReopen) {
  ScopedTempDir dir;
  const auto target = dir / "a.pdf";
  std::filesystem::copy_file(testdata("simple.pdf"), target);
  auto doc = PdfiumDocument::open(target);
  ASSERT_TRUE(doc) << doc.error().message;
  ASSERT_TRUE(doc->set_page_rotation(0, 1));

  auto saved = save_document_safely(*doc, target, {});
  ASSERT_TRUE(saved) << saved.error().message;
  EXPECT_FALSE(saved->incremental);
  EXPECT_GT(saved->bytes, 0u);
  EXPECT_EQ(files_in(dir.path()), 1u) << "temp file must not remain";

  auto reopened = PdfiumDocument::open(target);
  ASSERT_TRUE(reopened) << reopened.error().message;
  EXPECT_EQ(reopened->page_count(), 2);
  EXPECT_EQ(*reopened->page_rotation(0), 1);
  EXPECT_DOUBLE_EQ(reopened->page_size(0)->width, 792.0);
}

TEST(SaveDocument, VerificationFailureKeepsOriginal) {
  ScopedTempDir dir;
  const auto target = dir / "keep.pdf";
  std::filesystem::copy_file(testdata("simple.pdf"), target);
  const auto original = read_bytes(target);
  auto doc = PdfiumDocument::open(target);
  ASSERT_TRUE(doc);
  ASSERT_TRUE(doc->set_page_rotation(0, 2));

  SaveOptions options;
  options.after_write_for_testing = [](const std::filesystem::path& temp) {
    std::filesystem::resize_file(temp, 64);  // 저장 결과를 망가뜨린다
  };
  auto saved = save_document_safely(*doc, target, options);
  ASSERT_FALSE(saved);
  EXPECT_EQ(saved.error().code, ErrorCode::kSaveVerifyFailed);
  EXPECT_EQ(read_bytes(target), original) << "original must be untouched";
  EXPECT_EQ(files_in(dir.path()), 1u) << "temp file must be removed";
}

TEST(SaveDocument, MissingFolderFailsWithoutCreatingFiles) {
  ScopedTempDir dir;
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  auto saved = save_document_safely(*doc, dir.path() / "no-such-folder" / "x.pdf", {});
  ASSERT_FALSE(saved);
  EXPECT_EQ(saved.error().code, ErrorCode::kIoError);
  EXPECT_EQ(files_in(dir.path()), 0u);
}

TEST(SaveDocument, KoreanPathSaveAs) {
  ScopedTempDir dir;
  const auto folder = dir / "한글 폴더";
  std::filesystem::create_directories(folder);
  const auto target = folder / onpdf::path_from_utf8("저장 테스트.pdf");
  auto doc = PdfiumDocument::open(testdata("korean-embedded.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  auto saved = save_document_safely(*doc, target, {});
  ASSERT_TRUE(saved) << saved.error().message;
  auto reopened = PdfiumDocument::open(target);
  ASSERT_TRUE(reopened) << reopened.error().message;
  auto text = reopened->load_text(0);
  ASSERT_TRUE(text);
  EXPECT_GT(text->char_count(), 0);
}

// 암호화 문서는 암호화를 유지한 채 저장되고 같은 암호로 다시 열린다
TEST(SaveDocument, EncryptedDocumentsStayEncrypted) {
  for (const char* name : {"encrypted-rc4-user.pdf", "encrypted-aes256.pdf"}) {
    SCOPED_TRACE(name);
    ScopedTempDir dir;
    const auto target = dir / "enc.pdf";
    auto doc = PdfiumDocument::open(testdata(name), "user");
    ASSERT_TRUE(doc) << doc.error().message;
    ASSERT_TRUE(doc->set_page_rotation(0, 1));
    auto saved = save_document_safely(*doc, target, {});
    ASSERT_TRUE(saved) << saved.error().message;
    auto without = PdfiumDocument::open(target);
    ASSERT_FALSE(without);
    EXPECT_EQ(without.error().code, ErrorCode::kPasswordRequired);
    auto with = PdfiumDocument::open(target, "user");
    ASSERT_TRUE(with) << with.error().message;
    EXPECT_EQ(*with->page_rotation(0), 1);
  }
}

// 서명 문서는 증분 저장: 원본 바이트를 그대로 앞에 두고 변경만 덧붙인다
TEST(SaveDocument, IncrementalSaveKeepsSignedBytes) {
  ScopedTempDir dir;
  const auto target = dir / "signed.pdf";
  std::filesystem::copy_file(testdata("signed-dummy.pdf"), target);
  const auto original = read_bytes(target);
  auto doc = PdfiumDocument::open(target);
  ASSERT_TRUE(doc);
  EXPECT_EQ(doc->signature_count(), 1);
  ASSERT_TRUE(doc->set_page_rotation(0, 1));
  SaveOptions options;
  options.incremental = true;
  auto saved = save_document_safely(*doc, target, options);
  ASSERT_TRUE(saved) << saved.error().message;
  EXPECT_TRUE(saved->incremental);
  const auto after = read_bytes(target);
  ASSERT_GT(after.size(), original.size());
  EXPECT_TRUE(std::equal(original.begin(), original.end(), after.begin())) << "signed bytes must be preserved";
  auto reopened = PdfiumDocument::open(target);
  ASSERT_TRUE(reopened);
  EXPECT_EQ(reopened->signature_count(), 1);
  EXPECT_EQ(*reopened->page_rotation(0), 1);
}

TEST(SaveDocument, PermissionsReported) {
  auto plain = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(plain);
  EXPECT_TRUE(plain->can_modify());
  auto restricted = PdfiumDocument::open(testdata("encrypted-owner-only.pdf"));
  ASSERT_TRUE(restricted);
  EXPECT_FALSE(restricted->can_modify());  // P=-3904: 수정 금지
  auto owner = PdfiumDocument::open(testdata("encrypted-owner-only.pdf"), "owner");
  ASSERT_TRUE(owner);
  EXPECT_TRUE(owner->can_modify());
}

}  // namespace
}  // namespace onpdf::engine
