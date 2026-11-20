#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/qpdf_tools.h"
#include "onpdf/core/engine/save.h"
#include "onpdf/edit/add_text_command.h"

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

// 파일은 쓸 수 있어도 폴더에 새 파일을 만들 수 없으면: 임시 파일을 만들지 못하므로 폴더 문제로 알린다 (#31 리뷰 I4).
// Windows 는 폴더의 읽기 전용 속성이 파일 생성을 막지 않으므로 probe 가 성공해 건너뛴다
TEST(SaveDocument, ReadOnlyFolderIsRefused) {
  ScopedTempDir dir;
  namespace fs = std::filesystem;
  const auto folder = dir / "locked";
  fs::create_directory(folder);
  const auto target = folder / "doc.pdf";
  fs::copy_file(testdata("simple.pdf"), target);
  const auto original = read_bytes(target);
  fs::permissions(folder, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
  struct Restore {  // 폴더 정리를 위해 쓰기 권한을 되돌린다
    fs::path path;
    ~Restore() {
      std::error_code ec;
      fs::permissions(path, fs::perms::owner_all, fs::perm_options::add, ec);
    }
  } restore{folder};
  {
    const auto probe_path = folder / "probe.tmp";
    std::ofstream probe(probe_path, std::ios::binary);
    if (probe) {
      probe.close();
      std::error_code ec;
      fs::remove(probe_path, ec);
      GTEST_SKIP() << "folder write protection is not enforced here";
    }
  }
  auto doc = PdfiumDocument::open(target);
  ASSERT_TRUE(doc);
  ASSERT_TRUE(doc->set_page_rotation(0, 1));
  auto saved = save_document_safely(*doc, target, {});
  ASSERT_FALSE(saved);
  EXPECT_EQ(saved.error().code, ErrorCode::kFolderNotWritable) << saved.error().message;
  EXPECT_EQ(read_bytes(target), original);
  EXPECT_EQ(files_in(folder), 1u);
}

// write_files_safely 골격: 쓰기·검증 실패 시 기존 대상 불변, 임시 0개. 성공 시 여러 대상이 함께 바뀐다 (#37)
TEST(SaveDocument, WriteFilesSafelySkeleton) {
  ScopedTempDir dir;
  const auto a = dir / "a.txt";
  const auto b = dir / "b.txt";
  { std::ofstream(a) << "old-a"; }
  SafeWriteHooks ok;
  ok.write = [](const std::vector<std::filesystem::path>& temps) -> Result<void> {
    for (const auto& t : temps) std::ofstream(t) << "new";
    return {};
  };
  auto r = write_files_safely({a, b}, ok);
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(r->size(), 2u);
  EXPECT_EQ(files_in(dir.path()), 2u);
  EXPECT_EQ(read_bytes(a), read_bytes(b));
  SafeWriteHooks failing_write;
  failing_write.write = [](const std::vector<std::filesystem::path>&) -> Result<void> { return make_error(ErrorCode::kIoError, "disk"); };
  const auto before = read_bytes(a);
  EXPECT_FALSE(write_files_safely({a}, failing_write));
  EXPECT_EQ(read_bytes(a), before);
  SafeWriteHooks failing_verify = ok;
  failing_verify.verify = [](std::size_t, const std::filesystem::path&, const std::filesystem::path&) -> Result<void> {
    return make_error(ErrorCode::kSaveVerifyFailed, "x");
  };
  EXPECT_FALSE(write_files_safely({a, b}, failing_verify));
  EXPECT_EQ(read_bytes(a), before);
  EXPECT_EQ(files_in(dir.path()), 2u) << "임시 파일 없음";
  EXPECT_FALSE(write_files_safely({a, a}, ok)) << "같은 대상 두 번";
  EXPECT_FALSE(write_files_safely({dir / "new.txt", dir.path() / "." / "new.txt"}, ok)) << "다른 표기의 같은 새 파일 (리뷰 I3)";
  EXPECT_FALSE(write_files_safely({}, ok));
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
  EXPECT_TRUE(no_assemble->edit_permissions().extract);

  // 추출(비트 5)만 금지: 삽입 소스로 쓸 수 없다. 나머지는 허용
  auto no_extract = PdfiumDocument::open(testdata("perm-no-extract.pdf"));
  ASSERT_TRUE(no_extract) << no_extract.error().message;
  EXPECT_FALSE(no_extract->edit_permissions().extract);
  EXPECT_TRUE(no_extract->edit_permissions().assemble);
  EXPECT_FALSE(none.extract);
  EXPECT_TRUE(all.extract);
}


// ---- 미참조 폰트 정리 (#61) ----

std::shared_ptr<const edit::FontFace> cleanup_font() {
  auto face = edit::FontFace::load(testdata("fonts") / "NotoSansKR-subset.ttf");
  EXPECT_TRUE(face);
  return face ? *face : nullptr;
}

edit::AddTextCommand text_command(const std::string& utf8, double y = 100) {
  edit::TextBoxSpec spec;
  spec.page = 0;
  spec.origin = {72, y};
  spec.utf8 = utf8;
  spec.face = cleanup_font();
  return edit::AddTextCommand(spec);
}

// 추가 → 취소 → 저장: 폰트가 빠져 원본 크기 수준, 캐시는 그대로
TEST(SaveDocument, UndoneTextIsCleanedOnSave) {
  ScopedTempDir dir;
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  auto cmd = text_command("한글 텍스트");
  ASSERT_TRUE(cmd.execute(*doc));
  ASSERT_TRUE(cmd.undo(*doc));
  const auto target = dir / "undone.pdf";
  auto saved = save_document_safely(*doc, target, {});
  ASSERT_TRUE(saved) << saved.error().message;
  EXPECT_TRUE(saved->fonts_cleaned);
  EXPECT_LT(saved->bytes, std::filesystem::file_size(testdata("simple.pdf")) + 2000);
  EXPECT_EQ(doc->font_cache_size(), 1) << "엔진 캐시는 유지(재실행 때 재사용)";
  ASSERT_TRUE(PdfiumDocument::open(target));
  EXPECT_EQ(files_in(dir.path()), 1u) << "임시 파일 없음";
}

// 취소 1 + 남김 1: 남긴 텍스트의 폰트·렌더·추출·마크 id 는 그대로
TEST(SaveDocument, KeptTextSurvivesCleanup) {
  ScopedTempDir dir;
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  auto undone = text_command("첫 줄");
  ASSERT_TRUE(undone.execute(*doc));
  ASSERT_TRUE(undone.undo(*doc));
  auto kept = text_command("둘째 줄 텍스트", 140);
  ASSERT_TRUE(kept.execute(*doc));
  ASSERT_EQ(doc->font_cache_size(), 2);
  RenderOptions render;
  render.scale = 2.0f;
  auto before = doc->render_page(0, render);
  ASSERT_TRUE(before);
  const auto target = dir / "kept.pdf";
  auto saved = save_document_safely(*doc, target, {});
  ASSERT_TRUE(saved) << saved.error().message;
  EXPECT_TRUE(saved->fonts_cleaned);
  auto reopened = PdfiumDocument::open(target);
  ASSERT_TRUE(reopened);
  auto after = reopened->render_page(0, render);
  ASSERT_TRUE(after);
  EXPECT_EQ(after->pixels, before->pixels) << "저장 전후 렌더 동일";
  auto objects = reopened->text_objects(0);
  ASSERT_TRUE(objects);
  ASSERT_EQ(objects->size(), 1u);
  EXPECT_EQ((*objects)[0].id, kept.object_ids()[0]);
  auto text = reopened->load_text(0);
  ASSERT_TRUE(text);
  EXPECT_NE(text->all_text().find("둘째 줄 텍스트"), std::string::npos);
  EXPECT_TRUE(QpdfTools::check(target));
}

// 폰트가 전부 쓰이거나 텍스트를 넣은 적이 없으면 재작성하지 않는다 (PDFium 산출물 그대로)
TEST(SaveDocument, NoCleanupWhenNothingUnreferenced) {
  ScopedTempDir dir;
  {
    auto doc = PdfiumDocument::open(testdata("simple.pdf"));
    ASSERT_TRUE(doc);
    auto cmd = text_command("한글 텍스트");
    ASSERT_TRUE(cmd.execute(*doc));
    const auto direct = dir / "direct.pdf";
    ASSERT_TRUE(doc->save_to(direct, false));
    const auto target = dir / "all-used.pdf";
    auto saved = save_document_safely(*doc, target, {});
    ASSERT_TRUE(saved) << saved.error().message;
    EXPECT_FALSE(saved->fonts_cleaned);
    // PDFium 저장은 매번 /ID·날짜가 달라 바이트 비교는 못 한다: 크기가 같고 폰트가 남아 있으면 재작성하지 않은 것
    EXPECT_EQ(std::filesystem::file_size(target), std::filesystem::file_size(direct)) << "재작성 없음: PDFium 산출물 그대로";
    EXPECT_GT(saved->bytes, std::filesystem::file_size(testdata("simple.pdf")) + 2000) << "쓰이는 폰트는 남는다";
  }
  {
    auto doc = PdfiumDocument::open(testdata("simple.pdf"));
    ASSERT_TRUE(doc);
    ASSERT_TRUE(doc->set_page_rotation(0, 1));
    auto saved = save_document_safely(*doc, dir / "rotated.pdf", {});
    ASSERT_TRUE(saved);
    EXPECT_FALSE(saved->fonts_cleaned);
  }
}

TEST(SaveDocument, EncryptedCleanupKeepsPassword) {
  ScopedTempDir dir;
  auto doc = PdfiumDocument::open(testdata("encrypted-aes256.pdf"), "owner");
  ASSERT_TRUE(doc);
  auto cmd = text_command("한글 텍스트");
  ASSERT_TRUE(cmd.execute(*doc));
  ASSERT_TRUE(cmd.undo(*doc));
  const auto target = dir / "enc.pdf";
  auto saved = save_document_safely(*doc, target, {});
  ASSERT_TRUE(saved) << saved.error().message;
  EXPECT_TRUE(saved->fonts_cleaned);
  auto no_password = PdfiumDocument::open(target);
  ASSERT_FALSE(no_password);
  EXPECT_EQ(no_password.error().code, ErrorCode::kPasswordRequired);
  ASSERT_TRUE(PdfiumDocument::open(target, "user"));
  ASSERT_TRUE(PdfiumDocument::open(target, "owner"));
}

TEST(SaveDocument, SignedIncrementalSkipsCleanup) {
  ScopedTempDir dir;
  const auto target = dir / "signed.pdf";
  std::filesystem::copy_file(testdata("signed-dummy.pdf"), target);
  const auto original = read_bytes(target);
  auto doc = PdfiumDocument::open(target);
  ASSERT_TRUE(doc);
  auto cmd = text_command("한글 텍스트");
  ASSERT_TRUE(cmd.execute(*doc));
  ASSERT_TRUE(cmd.undo(*doc));
  SaveOptions options;
  options.incremental = true;
  auto saved = save_document_safely(*doc, target, options);
  ASSERT_TRUE(saved) << saved.error().message;
  EXPECT_FALSE(saved->fonts_cleaned);
  const auto bytes = read_bytes(target);
  ASSERT_GT(bytes.size(), original.size());
  EXPECT_TRUE(std::equal(original.begin(), original.end(), bytes.begin())) << "원본 바이트 보존";
}

// 재작성 실패는 저장 실패가 아니다: PDFium 산출물(폰트 남음)로 저장한다
TEST(SaveDocument, RewriteFailureFallsBackToPdfiumOutput) {
  ScopedTempDir dir;
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  auto cmd = text_command("한글 텍스트");
  ASSERT_TRUE(cmd.execute(*doc));
  ASSERT_TRUE(cmd.undo(*doc));
  SaveOptions options;
  int calls = 0;
  options.rewrite_for_testing = [&](const std::filesystem::path&, const std::filesystem::path&, std::string_view) -> Result<void> {
    ++calls;
    return make_error(ErrorCode::kJobFailed, "injected");
  };
  const auto target = dir / "fallback.pdf";
  auto saved = save_document_safely(*doc, target, options);
  ASSERT_TRUE(saved) << saved.error().message;
  EXPECT_EQ(calls, 1);
  EXPECT_FALSE(saved->fonts_cleaned);
  EXPECT_GT(saved->bytes, std::filesystem::file_size(testdata("simple.pdf")) + 2000) << "폰트가 남은 채 저장";
  ASSERT_TRUE(PdfiumDocument::open(target));
  EXPECT_EQ(files_in(dir.path()), 1u) << "두 번째 임시 파일도 정리";
}

// 재작성이 문서를 망가뜨리면 기존 검증이 잡아 원본은 그대로
TEST(SaveDocument, RewriteCorruptionIsCaughtByVerify) {
  ScopedTempDir dir;
  const auto target = dir / "doc.pdf";
  std::filesystem::copy_file(testdata("simple.pdf"), target);
  const auto original = read_bytes(target);
  auto doc = PdfiumDocument::open(target);
  ASSERT_TRUE(doc);
  auto cmd = text_command("한글 텍스트");
  ASSERT_TRUE(cmd.execute(*doc));
  ASSERT_TRUE(cmd.undo(*doc));
  SaveOptions options;
  options.rewrite_for_testing = [](const std::filesystem::path&, const std::filesystem::path& out, std::string_view) -> Result<void> {
    std::ofstream(out, std::ios::binary) << "%PDF-1.7 broken";
    return {};
  };
  auto saved = save_document_safely(*doc, target, options);
  ASSERT_FALSE(saved);
  EXPECT_EQ(saved.error().code, ErrorCode::kSaveVerifyFailed);
  EXPECT_EQ(read_bytes(target), original) << "원본 불변";
  EXPECT_EQ(files_in(dir.path()), 1u) << "임시 파일 모두 제거";
}
}  // namespace
}  // namespace onpdf::engine
