// 코퍼스 왕복 테스트 (#119): testdata/corpus.tsv 의 공개 PDF(라이선스 확인, 커밋 SHA·sha256 고정)를
// `python3 testdata/gen/fetch_corpus.py` 로 받아 두면 파일마다 열기 → 안전 저장 → 다시 열기 비교 →
// qpdf 검사를 돈다(검사 본문은 roundtrip_checks.h). 계획서 성공 기준 "200개 이상에서 깨지는 파일
// 0건". 파일이 없으면 SKIP, ONPDF_CORPUS_REQUIRED 가 설정돼 있으면(CI) FAIL.

#include "onpdf/core/engine/pdfium_document.h"

#include "../manifest.h"
#include "../test_support.h"
#include "roundtrip_checks.h"
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace onpdf::engine {
namespace {

using onpdf::test::parse_number;
using onpdf::test::split_tabs;
using onpdf::test::testdata;

struct CorpusEntry {
  std::string name;
  std::string source;
  std::string path;
  std::string sha256;
  std::uint64_t size = 0;
  std::string expect;  // ok | password | invalid
  std::string password;
  std::string note;
};

struct Corpus {
  std::vector<CorpusEntry> entries;
  std::vector<std::string> bad_rows;
};

// 정적 초기화 시점에 호출되므로 절대 던지지 않는다. 잘못된 행은 bad_rows 에 기록만 한다.
const Corpus& corpus() {
  static const Corpus c = [] {
    Corpus result;
    std::ifstream in(testdata("corpus.tsv"));
    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
      ++line_no;
      if (line.empty() || line[0] == '#') continue;
      auto cols = split_tabs(line);
      if (cols[0] == "name") continue;
      if (cols.size() != 8) {
        result.bad_rows.push_back("line " + std::to_string(line_no) + ": expected 8 columns");
        continue;
      }
      CorpusEntry e;
      e.name = cols[0];
      e.source = cols[1];
      e.path = cols[2];
      e.sha256 = cols[3];
      if (!parse_number(cols[4], e.size)) {
        result.bad_rows.push_back("line " + std::to_string(line_no) + ": size not numeric");
        continue;
      }
      e.expect = cols[5];
      e.password = cols[6];
      e.note = cols[7];
      result.entries.push_back(std::move(e));
    }
    return result;
  }();
  return c;
}

std::filesystem::path corpus_file(const CorpusEntry& e) {
  return testdata("corpus") / path_from_utf8(e.source) / path_from_utf8(e.name);
}

// gtest 파라미터 이름: 영숫자 외는 '_'. 두 파일이 같은 이름이 되면 gtest 가 실행 때 중단하므로
// CorpusList.IsWellFormed 가 유일성을 검사한다
std::string gtest_name_of(const CorpusEntry& e) {
  std::string name;
  for (char c : e.source + "_" + e.name) {
    const auto ch = static_cast<unsigned char>(c);
    name += std::isalnum(ch) ? static_cast<char>(ch) : '_';
  }
  return name;
}

std::string corpus_test_name(const ::testing::TestParamInfo<CorpusEntry>& info) {
  return gtest_name_of(info.param);
}

bool corpus_required() {
  const char* env = std::getenv("ONPDF_CORPUS_REQUIRED");
  return env != nullptr && *env != '\0' && std::string(env) != "0";
}

class CorpusRoundtrip : public ::testing::TestWithParam<CorpusEntry> {};

TEST_P(CorpusRoundtrip, SaveAndReopenMatches) {
  const CorpusEntry& e = GetParam();
  const auto file = corpus_file(e);
  const std::string label = e.name + " [" + e.source + "]";
  if (!std::filesystem::is_regular_file(file)) {
    if (corpus_required()) {
      FAIL() << label << " stage=fetch: file missing — run python3 testdata/gen/fetch_corpus.py";
    }
    GTEST_SKIP() << "corpus file not downloaded (python3 testdata/gen/fetch_corpus.py)";
  }

  if (e.expect == "invalid") {
    auto doc = PdfiumDocument::open(file, e.password);
    EXPECT_FALSE(doc) << label << " stage=open: expected to fail but opened";
    return;
  }
  if (e.expect == "password") {
    auto without = PdfiumDocument::open(file);
    ASSERT_FALSE(without) << label << " stage=open: opened without a password";
    EXPECT_EQ(without.error().code, ErrorCode::kPasswordRequired) << label << " stage=open";
  }
  onpdf::test::expect_roundtrip(file, e.password, label);
}

INSTANTIATE_TEST_SUITE_P(Corpus, CorpusRoundtrip, ::testing::ValuesIn(corpus().entries),
                         corpus_test_name);

// 목록 자체: 행 형식, 200개 이상, 출처 안 이름 유일, 암호 행의 암호, 알려진 출처
TEST(CorpusList, IsWellFormed) {
  for (const auto& row : corpus().bad_rows) ADD_FAILURE() << "corpus.tsv " << row;
  EXPECT_GE(corpus().entries.size(), 200u);
  static const std::set<std::string> kSources = {"pdfium", "qpdf", "pdfjs", "pdfbox"};
  static const std::set<std::string> kExpects = {"ok", "password", "invalid"};
  std::set<std::pair<std::string, std::string>> names;
  std::set<std::string> gtest_names;
  for (const auto& e : corpus().entries) {
    EXPECT_TRUE(gtest_names.insert(gtest_name_of(e)).second)
        << e.source << "/" << e.name << ": gtest name collides with another file";
    EXPECT_TRUE(kSources.contains(e.source)) << e.name << ": unknown source " << e.source;
    EXPECT_TRUE(kExpects.contains(e.expect)) << e.name << ": unknown expect " << e.expect;
    EXPECT_EQ(e.sha256.size(), 64u) << e.name;
    EXPECT_GT(e.size, 0u) << e.name;
    if (e.expect == "password") EXPECT_FALSE(e.password.empty()) << e.name << ": no password";
    EXPECT_TRUE(names.insert({e.source, e.name}).second)
        << e.source << "/" << e.name << ": duplicate name";
  }
}

}  // namespace
}  // namespace onpdf::engine
