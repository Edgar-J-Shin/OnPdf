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

// 원본 권한을 유지한다: 본인만 읽을 수 있던 문서가 저장 뒤 다른 사용자에게 열리면 안 된다 (#31 리뷰 C1)
TEST(SaveDocument, PermissionsPreserved) {
  ScopedTempDir dir;
  const auto target = dir / "private.pdf";
  std::filesystem::copy_file(testdata("simple.pdf"), target);
  std::filesystem::permissions(target, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace);
  const auto before = std::filesystem::status(target).permissions();
  auto doc = PdfiumDocument::open(target);
  ASSERT_TRUE(doc);
  ASSERT_TRUE(doc->set_page_rotation(0, 1));
  ASSERT_TRUE(save_document_safely(*doc, target, {}));
  EXPECT_EQ(std::filesystem::status(target).permissions(), before);
}

// 읽기 전용 파일은 몰래 덮어쓰지 않는다: 쓸 수 없음으로 거절하고 원본·폴더를 그대로 둔다 (#31 리뷰 C1)
TEST(SaveDocument, ReadOnlyFileIsRefused) {
  ScopedTempDir dir;
  const auto target = dir / "readonly.pdf";
  std::filesystem::copy_file(testdata("simple.pdf"), target);
  const auto original = read_bytes(target);
  namespace fs = std::filesystem;
  fs::permissions(target, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read,
                  fs::perm_options::replace);
  struct Restore {  // 폴더 정리를 위해 쓰기 권한을 되돌린다 (Windows 는 읽기 전용 파일을 지우지 못한다)
    fs::path path;
    ~Restore() {
      std::error_code ec;
      fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::add, ec);
    }
  } restore{target};
  if (std::ofstream probe(target, std::ios::binary | std::ios::app); probe) {
    GTEST_SKIP() << "running with privileges that can write read-only files";
  }
  auto doc = PdfiumDocument::open(target);
  ASSERT_TRUE(doc);
  ASSERT_TRUE(doc->set_page_rotation(0, 1));
  auto saved = save_document_safely(*doc, target, {});
  ASSERT_FALSE(saved);
  EXPECT_EQ(saved.error().code, ErrorCode::kNotWritable);
  EXPECT_EQ(read_bytes(target), original);
  EXPECT_EQ(files_in(dir.path()), 1u);
}

// 심볼릭 링크로 연 문서: 링크는 그대로 두고 링크가 가리키는 실제 파일을 갱신한다 (#31 리뷰 C1)
TEST(SaveDocument, SymlinkIsKeptAndRealFileUpdated) {
  ScopedTempDir dir;
  const auto real = dir / "real.pdf";
  const auto link = dir / "link.pdf";
  std::filesystem::copy_file(testdata("simple.pdf"), real);
  std::error_code ec;
  std::filesystem::create_symlink(real, link, ec);
  if (ec) GTEST_SKIP() << "cannot create symlinks here: " << ec.message();
  auto doc = PdfiumDocument::open(link);
  ASSERT_TRUE(doc);
  ASSERT_TRUE(doc->set_page_rotation(0, 1));
  auto saved = save_document_safely(*doc, link, {});
  ASSERT_TRUE(saved) << saved.error().message;
  EXPECT_EQ(saved->path, link);
  EXPECT_TRUE(std::filesystem::is_symlink(std::filesystem::symlink_status(link)));
  auto reopened = PdfiumDocument::open(real);
  ASSERT_TRUE(reopened);
  EXPECT_EQ(*reopened->page_rotation(0), 1);
  EXPECT_EQ(files_in(dir.path()), 2u);  // real + link, 임시 없음
}

TEST(SaveDocument, EditPermissionsByKind) {
  auto plain = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(plain);
  const EditPermissions all = plain->edit_permissions();
  EXPECT_TRUE(all.modify_contents && all.annotate && all.fill_forms && all.assemble);

  auto restricted = PdfiumDocument::open(testdata("encrypted-owner-only.pdf"));  // P=-3904: 전부 금지
  ASSERT_TRUE(restricted);
  const EditPermissions none = restricted->edit_permissions();
  EXPECT_FALSE(none.modify_contents);
  EXPECT_FALSE(none.annotate);
  EXPECT_FALSE(none.fill_forms);
  EXPECT_FALSE(none.assemble);
  auto owner = PdfiumDocument::open(testdata("encrypted-owner-only.pdf"), "owner");  // 소유자 암호: 제한 해제
  ASSERT_TRUE(owner);
  EXPECT_TRUE(owner->edit_permissions().assemble);

  // 쪽 조립만 허용(비트 11 설정·비트 4 해제): 회전은 되고 내용 수정은 안 된다
  auto assemble_only = PdfiumDocument::open(testdata("perm-assemble-only.pdf"));
  ASSERT_TRUE(assemble_only) << assemble_only.error().message;
  EXPECT_TRUE(assemble_only->edit_permissions().assemble);
  EXPECT_FALSE(assemble_only->edit_permissions().modify_contents);
  EXPECT_TRUE(assemble_only->edit_permissions().annotate);

  // 내용 수정은 허용·조립 금지(R3 에서 비트 11 해제): 회전 불가
  auto no_assemble = PdfiumDocument::open(testdata("perm-no-assemble.pdf"));
  ASSERT_TRUE(no_assemble) << no_assemble.error().message;
  EXPECT_FALSE(no_assemble->edit_permissions().assemble);
  EXPECT_TRUE(no_assemble->edit_permissions().modify_contents);
}

}  // namespace
}  // namespace onpdf::engine
