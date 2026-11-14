// testdata/manifest.tsv 의 모든 파일을 열어 기대값(열림/암호/손상, 쪽 수, 첫 쪽 크기)과 비교한다.
// 새 테스트 PDF 는 manifest 에 한 줄 추가하면 자동으로 검증된다.

#include "onpdf/core/engine/pdfium_document.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace onpdf::engine {
namespace {

using onpdf::test::testdata;

struct ManifestEntry {
  std::string file;
  std::string expect;  // ok | password | invalid
  std::string password;
  std::string owner_password;
  int pages = 0;
  double width = 0.0;
  double height = 0.0;
  bool render = false;
};

std::vector<std::string> split_tabs(const std::string& line) {
  std::vector<std::string> cols;
  std::string cur;
  for (char ch : line) {
    if (ch == '\t') {
      cols.push_back(cur);
      cur.clear();
    } else if (ch != '\r') {
      cur += ch;
    }
  }
  cols.push_back(cur);
  return cols;
}

std::vector<ManifestEntry> load_manifest() {
  std::vector<ManifestEntry> entries;
  std::ifstream in(testdata("manifest.tsv"));
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    auto cols = split_tabs(line);
    if (cols.size() < 8 || cols[0] == "file") continue;
    ManifestEntry e;
    e.file = cols[0];
    e.expect = cols[1];
    e.password = cols[2];
    e.owner_password = cols[3];
    e.pages = std::stoi(cols[4]);
    e.width = std::stod(cols[5]);
    e.height = std::stod(cols[6]);
    e.render = cols[7] == "1";
    entries.push_back(std::move(e));
  }
  return entries;
}

// gtest 이름에는 영숫자·밑줄만 쓸 수 있다
std::string test_name(const ::testing::TestParamInfo<ManifestEntry>& info) {
  std::string name;
  for (char c : info.param.file) {
    const auto ch = static_cast<unsigned char>(c);
    name += std::isalnum(ch) ? static_cast<char>(ch) : '_';
  }
  if (name.find_first_not_of('_') == std::string::npos) return "non_ascii_name";  // 예: 한글 파일명.pdf
  return name;
}

void expect_opened(const ManifestEntry& e, const Result<PdfiumDocument>& doc) {
  ASSERT_TRUE(doc) << e.file << ": " << doc.error().message;
  EXPECT_EQ(doc->page_count(), e.pages) << e.file;
  if (e.pages > 0) {
    auto size = doc->page_size(0);
    ASSERT_TRUE(size) << e.file;
    EXPECT_NEAR(size->width, e.width, 0.5) << e.file;
    EXPECT_NEAR(size->height, e.height, 0.5) << e.file;
    RenderOptions options;
    options.scale = 0.25f;
    auto bitmap = doc->render_page(0, options);
    EXPECT_TRUE(bitmap) << e.file << ": " << (bitmap ? "" : bitmap.error().message);
  }
}

class ManifestTest : public ::testing::TestWithParam<ManifestEntry> {};

TEST_P(ManifestTest, OpensAsExpected) {
  const ManifestEntry& e = GetParam();
  auto doc = PdfiumDocument::open(testdata(e.file.c_str()));

  if (e.expect == "ok") {
    expect_opened(e, doc);
    if (!e.owner_password.empty()) {
      // 소유자 암호만 있는 문서: 소유자 암호로도 열려야 한다
      auto owner = PdfiumDocument::open(testdata(e.file.c_str()), e.owner_password);
      EXPECT_TRUE(owner) << e.file << " (owner password)";
    }
    return;
  }
  if (e.expect == "password") {
    ASSERT_FALSE(doc) << e.file << ": opened without password";
    EXPECT_EQ(doc.error().code, ErrorCode::kPasswordRequired) << e.file;
    auto wrong = PdfiumDocument::open(testdata(e.file.c_str()), "wrong-password");
    ASSERT_FALSE(wrong) << e.file;
    EXPECT_EQ(wrong.error().code, ErrorCode::kPasswordRequired) << e.file;
    auto user = PdfiumDocument::open(testdata(e.file.c_str()), e.password);
    expect_opened(e, user);
    if (!e.owner_password.empty()) {
      auto owner = PdfiumDocument::open(testdata(e.file.c_str()), e.owner_password);
      expect_opened(e, owner);
    }
    return;
  }
  if (e.expect == "invalid") {
    ASSERT_FALSE(doc) << e.file << ": opened unexpectedly";
    EXPECT_EQ(doc.error().code, ErrorCode::kInvalidFormat) << e.file;
    return;
  }
  FAIL() << e.file << ": unknown expect '" << e.expect << "'";
}

INSTANTIATE_TEST_SUITE_P(Testdata, ManifestTest, ::testing::ValuesIn(load_manifest()), test_name);

TEST(Testdata, ManifestIsNotEmpty) { EXPECT_GE(load_manifest().size(), 20u); }

TEST(Testdata, LargeDocumentOpensQuickly) {
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  auto doc = PdfiumDocument::open(testdata("large-1000.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  EXPECT_EQ(doc->page_count(), 1000);
  const auto t1 = clock::now();
  RenderOptions options;
  options.scale = 1.0f;
  auto bitmap = doc->render_page(999, options);
  ASSERT_TRUE(bitmap) << bitmap.error().message;
  const auto t2 = clock::now();
  const auto open_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
  const auto render_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
  RecordProperty("open_ms", static_cast<int>(open_ms));
  RecordProperty("render_last_page_ms", static_cast<int>(render_ms));
  std::cout << "[ perf     ] large-1000.pdf open " << open_ms << " ms, render page 1000 " << render_ms
            << " ms\n";
  EXPECT_LT(open_ms, 10000);
}

}  // namespace
}  // namespace onpdf::engine
