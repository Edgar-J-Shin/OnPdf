// QpdfTools: 병합(주 입력 목차 보존)·추출·검사·쪽 수·권한 — write_files_safely 와 함께 (#37)
#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/qpdf_tools.h"
#include "onpdf/core/engine/save.h"
#include "onpdf/core/platform/path.h"
#include "onpdf/edit/add_text_command.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace onpdf::engine {
namespace {

using onpdf::test::ScopedTempDir;
using onpdf::test::testdata;

std::size_t files_in(const std::filesystem::path& dir) {
  std::size_t n = 0;
  for ([[maybe_unused]] const auto& e : std::filesystem::directory_iterator(dir)) ++n;
  return n;
}

// 병합 결과를 임시→검증→교체로 쓰는 호출자 쪽 조립 (앱·CLI 가 쓰는 것과 같은 모양)
Result<std::vector<SaveResult>> merge_safely(const std::vector<StructureInput>& inputs, const std::filesystem::path& out,
                                             int expected_pages) {
  SafeWriteHooks hooks;
  hooks.write = [&](const std::vector<std::filesystem::path>& temps) -> Result<void> {
    auto r = QpdfTools::merge(inputs, temps.front());
    if (!r) return std::unexpected(r.error());
    return {};
  };
  hooks.verify = [&](std::size_t, const std::filesystem::path&, const std::filesystem::path& temp) -> Result<void> {
    if (auto c = QpdfTools::check(temp); !c) return std::unexpected(c.error());
    auto n = QpdfTools::page_count(temp);
    if (!n) return std::unexpected(n.error());
    if (*n != expected_pages) return make_error(ErrorCode::kSaveVerifyFailed, "page count mismatch");
    return {};
  };
  return write_files_safely({out}, hooks);
}

TEST(QpdfTools, PageCountAndPermissions) {
  EXPECT_EQ(*QpdfTools::page_count(testdata("bookmarks.pdf")), 10);
  EXPECT_EQ(*QpdfTools::page_count(testdata("encrypted-aes256.pdf"), "user"), 2);
  auto locked = QpdfTools::page_count(testdata("encrypted-aes256.pdf"));
  ASSERT_FALSE(locked);
  EXPECT_EQ(locked.error().code, ErrorCode::kPasswordRequired);
  auto missing = QpdfTools::page_count(testdata("nope.pdf"));
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().code, ErrorCode::kFileNotFound);
  EXPECT_TRUE(*QpdfTools::allows_extraction(testdata("simple.pdf")));
  EXPECT_FALSE(*QpdfTools::allows_extraction(testdata("perm-no-extract.pdf")));
  EXPECT_TRUE(*QpdfTools::allows_extraction(testdata("perm-no-assemble.pdf")));
}

TEST(QpdfTools, CheckReportsWarningsAndErrors) {
  auto ok = QpdfTools::check(testdata("simple.pdf"));
  ASSERT_TRUE(ok) << ok.error().message;
  EXPECT_TRUE(ok->warnings.empty());
  auto damaged = QpdfTools::check(testdata("bad-xref.pdf"));  // 복구 가능: 경고와 함께 성공
  ASSERT_TRUE(damaged) << damaged.error().message;
  EXPECT_FALSE(damaged->warnings.empty());
  auto broken = QpdfTools::check(testdata("truncated.pdf"));
  EXPECT_FALSE(broken);
  auto encrypted = QpdfTools::check(testdata("encrypted-aes256.pdf"), "user");
  EXPECT_TRUE(encrypted) << encrypted.error().message;
}

// 주 입력(첫 파일)의 목차가 보존되고 한글·암호 입력이 섞여도 쪽이 순서대로 이어진다
TEST(QpdfTools, MergeKeepsPrimaryOutlineAndPages) {
  ScopedTempDir dir;
  const auto out = dir / "merged.pdf";
  const std::vector<StructureInput> inputs{{testdata("bookmarks.pdf"), ""}, {testdata("encrypted-aes256.pdf"), "user"},
                                           {testdata("korean-embedded.pdf"), ""}};
  auto saved = merge_safely(inputs, out, 13);
  ASSERT_TRUE(saved) << saved.error().message;
  EXPECT_EQ(files_in(dir.path()), 1u) << "임시 파일이 남지 않는다";
  auto doc = PdfiumDocument::open(out);
  ASSERT_TRUE(doc);
  EXPECT_EQ(doc->page_count(), 13);
  auto outline = doc->load_outline();
  ASSERT_TRUE(outline);
  EXPECT_EQ(outline->size(), 15) << "첫 문서의 목차가 유지된다";
  auto korean = doc->load_text(12);
  ASSERT_TRUE(korean);
  bool non_ascii = false;
  for (char ch : korean->all_text()) non_ascii = non_ascii || static_cast<unsigned char>(ch) >= 0x80;
  EXPECT_TRUE(non_ascii);
  EXPECT_EQ(doc->signature_count(), 0);
}

TEST(QpdfTools, MergeRejectsBadInputsWithoutOutput) {
  ScopedTempDir dir;
  const auto out = dir / "merged.pdf";
  auto wrong_pw = merge_safely({{testdata("simple.pdf"), ""}, {testdata("encrypted-aes256.pdf"), "wrong"}}, out, 4);
  ASSERT_FALSE(wrong_pw);
  EXPECT_EQ(wrong_pw.error().code, ErrorCode::kPasswordRequired);
  EXPECT_NE(wrong_pw.error().message.find("encrypted-aes256.pdf"), std::string::npos);
  auto forbidden = merge_safely({{testdata("simple.pdf"), ""}, {testdata("perm-no-extract.pdf"), ""}}, out, 4);
  ASSERT_FALSE(forbidden);
  EXPECT_EQ(forbidden.error().code, ErrorCode::kPermissionDenied);
  auto broken = merge_safely({{testdata("simple.pdf"), ""}, {testdata("truncated.pdf"), ""}}, out, 2);
  EXPECT_FALSE(broken);
  EXPECT_FALSE(QpdfTools::merge({}, out));
  EXPECT_EQ(files_in(dir.path()), 0u) << "실패하면 결과 파일도 임시 파일도 없다";
  // 입력 = 출력
  const auto copy = dir / "a.pdf";
  std::filesystem::copy_file(testdata("simple.pdf"), copy);
  auto same = QpdfTools::merge({{copy, ""}}, copy);
  ASSERT_FALSE(same);
  EXPECT_EQ(same.error().code, ErrorCode::kInvalidArgument);
}

// 암호 문서가 주 입력(첫 파일)이어도 결과는 평문이라 암호 없이 검증·열기가 된다 (리뷰 C1)
TEST(QpdfTools, EncryptedPrimaryInputYieldsPlainOutput) {
  ScopedTempDir dir;
  const auto out = dir / "merged.pdf";
  auto saved = merge_safely({{testdata("encrypted-aes256.pdf"), "user"}, {testdata("simple.pdf"), ""}}, out, 4);
  ASSERT_TRUE(saved) << saved.error().message;
  EXPECT_EQ(*QpdfTools::page_count(out), 4);  // 암호 없이
  auto doc = PdfiumDocument::open(out);
  ASSERT_TRUE(doc) << "결과는 평문이어야 한다";
  EXPECT_EQ(doc->page_count(), 4);
  // 암호 문서에서 쪽 추출도 평문으로
  const auto piece = dir / "piece.pdf";
  ASSERT_TRUE(QpdfTools::extract({testdata("encrypted-aes256.pdf"), "user"}, {2, 2}, piece));
  EXPECT_EQ(*QpdfTools::page_count(piece), 1);
  EXPECT_TRUE(QpdfTools::check(piece));
}

// 파일 경로에 "password" 가 들어간 손상 파일은 암호 오류가 아니다 (리뷰 W1)
TEST(QpdfTools, DamagedFileNamedPasswordIsNotAPasswordError) {
  ScopedTempDir dir;
  const auto odd = dir / "my-passwords.pdf";
  std::filesystem::copy_file(testdata("truncated.pdf"), odd);
  auto count = QpdfTools::page_count(odd);
  ASSERT_FALSE(count);
  EXPECT_EQ(count.error().code, ErrorCode::kInvalidFormat);
  auto checked = QpdfTools::check(odd);
  ASSERT_FALSE(checked);
  EXPECT_EQ(checked.error().code, ErrorCode::kJobFailed);
  EXPECT_EQ(checked.error().message.rfind("qpdfjob json: ", 0), std::string::npos) << "접두가 정리된다: " << checked.error().message;
  // 진짜 암호 오류는 여전히 구분된다
  auto locked = QpdfTools::check(testdata("encrypted-aes256.pdf"));
  ASSERT_FALSE(locked);
  EXPECT_EQ(locked.error().code, ErrorCode::kPasswordRequired);
}

// 손상됐지만 복구되는 입력은 경고와 함께 병합된다
TEST(QpdfTools, MergeRecoversDamagedInputWithWarnings) {
  ScopedTempDir dir;
  const auto out = dir / "merged.pdf";
  auto report = QpdfTools::merge({{testdata("bad-xref.pdf"), ""}, {testdata("simple.pdf"), ""}}, out);
  ASSERT_TRUE(report) << report.error().message;
  EXPECT_FALSE(report->warnings.empty());
  EXPECT_EQ(*QpdfTools::page_count(out), 4);
}

// 분할: 범위마다 추출해 전부-또는-없음으로 쓴다
TEST(QpdfTools, ExtractRangesAllOrNothing) {
  ScopedTempDir dir;
  const StructureInput in{testdata("bookmarks.pdf"), ""};
  const auto ranges = ranges_every(3, 10);
  std::vector<std::filesystem::path> outs;
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    outs.push_back(dir.path() / path_from_utf8(output_name("bookmarks-%d.pdf", static_cast<int>(i) + 1,
                                                            static_cast<int>(ranges.size()), ranges[i], 10)));
  }
  SafeWriteHooks hooks;
  hooks.write = [&](const std::vector<std::filesystem::path>& temps) -> Result<void> {
    for (std::size_t i = 0; i < temps.size(); ++i) {
      if (auto r = QpdfTools::extract(in, ranges[i], temps[i]); !r) return std::unexpected(r.error());
    }
    return {};
  };
  int verified = 0;
  hooks.verify = [&](std::size_t i, const std::filesystem::path&, const std::filesystem::path& temp) -> Result<void> {
    auto n = QpdfTools::page_count(temp);
    if (!n) return std::unexpected(n.error());
    if (*n != ranges[i].last - ranges[i].first + 1) return make_error(ErrorCode::kSaveVerifyFailed, "pages");
    ++verified;
    return {};
  };
  auto saved = write_files_safely(outs, hooks);
  ASSERT_TRUE(saved) << saved.error().message;
  EXPECT_EQ(verified, 4);
  EXPECT_EQ(files_in(dir.path()), 4u);
  EXPECT_TRUE(std::filesystem::exists(dir / "bookmarks-1.pdf"));
  auto last = PdfiumDocument::open(dir / "bookmarks-4.pdf");
  ASSERT_TRUE(last);
  EXPECT_EQ(last->page_count(), 1);
  EXPECT_NE(last->load_text(0)->all_text().find("page 10 of 10"), std::string::npos);

  // 두 번째 조각 검증을 실패시키면 아무 파일도 바뀌지 않는다 (기존 4개는 그대로)
  ScopedTempDir dir2;
  std::vector<std::filesystem::path> outs2{dir2 / "a.pdf", dir2 / "b.pdf"};
  SafeWriteHooks failing;
  failing.write = hooks.write;
  failing.verify = [&](std::size_t, const std::filesystem::path& target, const std::filesystem::path&) -> Result<void> {
    if (target.filename() == "b.pdf") return make_error(ErrorCode::kSaveVerifyFailed, "injected");
    return {};
  };
  auto r2 = write_files_safely(outs2, failing);
  ASSERT_FALSE(r2);
  EXPECT_EQ(r2.error().code, ErrorCode::kSaveVerifyFailed);
  EXPECT_EQ(files_in(dir2.path()), 0u) << "실패하면 결과 0개, 임시 0개";
}


// 재작성 (#61): 미참조 객체 폐기, 암호 유지, 손상 복구 경고는 성공, 오류는 실패
TEST(QpdfTools, RewriteDropsUnreferencedAndKeepsEncryption) {
  ScopedTempDir dir;
  {
    // 평문: 텍스트 추가 → 제거 → PDFium 저장본에는 폰트가 남고, 재작성본에는 없다
    auto face = edit::FontFace::load(testdata("fonts") / "NotoSansKR-subset.ttf");
    ASSERT_TRUE(face);
    auto doc = PdfiumDocument::open(testdata("simple.pdf"));
    ASSERT_TRUE(doc);
    edit::TextBoxSpec spec;
    spec.page = 0;
    spec.origin = {72, 100};
    spec.utf8 = "한글 텍스트";
    spec.face = *face;
    edit::AddTextCommand add(spec);
    ASSERT_TRUE(add.execute(*doc));
    ASSERT_TRUE(add.undo(*doc));
    const auto bloated = dir / "bloated.pdf";
    ASSERT_TRUE(doc->save_to(bloated, false));
    const auto original_size = std::filesystem::file_size(testdata("simple.pdf"));
    EXPECT_GT(std::filesystem::file_size(bloated), original_size + 2000) << "PDFium 저장은 미참조 폰트를 남긴다";
    const auto slim = dir / "slim.pdf";
    auto r = QpdfTools::rewrite(bloated, slim);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_TRUE(r->warnings.empty());
    EXPECT_LT(std::filesystem::file_size(slim), original_size + 2000);
    auto check = QpdfTools::check(slim);
    ASSERT_TRUE(check);
    EXPECT_TRUE(check->warnings.empty());
    ASSERT_TRUE(PdfiumDocument::open(slim));
    EXPECT_FALSE(QpdfTools::rewrite(bloated, bloated)) << "같은 파일은 거절";
  }
  {
    // 암호: 결과도 같은 암호 문서, 권한 동일
    const auto out = dir / "enc.pdf";
    auto r = QpdfTools::rewrite(testdata("encrypted-aes256.pdf"), out, "user");
    ASSERT_TRUE(r) << r.error().message;
    auto no_password = PdfiumDocument::open(out);
    ASSERT_FALSE(no_password);
    EXPECT_EQ(no_password.error().code, ErrorCode::kPasswordRequired);
    auto with = PdfiumDocument::open(out, "user");
    ASSERT_TRUE(with) << with.error().message;
    auto before = PdfiumDocument::open(testdata("encrypted-aes256.pdf"), "user");
    ASSERT_TRUE(before);
    EXPECT_EQ(with->edit_permissions().assemble, before->edit_permissions().assemble);
    EXPECT_EQ(with->edit_permissions().annotate, before->edit_permissions().annotate);
    auto wrong = QpdfTools::rewrite(testdata("encrypted-aes256.pdf"), dir / "wrong.pdf", "nope");
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().code, ErrorCode::kPasswordRequired);
  }
  {
    auto recovered = QpdfTools::rewrite(testdata("bad-xref.pdf"), dir / "recovered.pdf");
    ASSERT_TRUE(recovered) << recovered.error().message;
    EXPECT_FALSE(recovered->warnings.empty()) << "복구 경고와 함께 성공";
    auto broken = QpdfTools::rewrite(testdata("truncated.pdf"), dir / "broken.pdf");
    ASSERT_FALSE(broken);
    EXPECT_EQ(broken.error().code, ErrorCode::kJobFailed);
  }
}
}  // namespace
}  // namespace onpdf::engine
