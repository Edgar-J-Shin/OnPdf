#pragma once

// testdata/manifest.tsv 읽기 — 매니페스트 테스트와 왕복 테스트가 함께 쓴다.

#include "test_support.h"

#include <gtest/gtest.h>

#include <cctype>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace onpdf::test {

struct ManifestEntry {
  std::string file;
  std::string expect;  // ok | password | invalid
  std::string password;
  std::string owner_password;
  int pages = 0;
  double width = 0.0;
  double height = 0.0;
  bool render = false;
  int chars = -1;  // 첫 쪽 글자 수. -1 = 검사 안 함
};

inline std::vector<std::string> split_tabs(const std::string& line) {
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

template <class T>
inline bool parse_number(const std::string& text, T& out) {
  const char* begin = text.data();
  const char* end = begin + text.size();
  auto [ptr, ec] = std::from_chars(begin, end, out);
  return ec == std::errc{} && ptr == end;
}

// 정적 초기화 시점에 호출되므로 절대 던지지 않는다. 잘못된 행은 bad_rows 에 기록만 한다.
struct Manifest {
  std::vector<ManifestEntry> entries;
  std::vector<std::string> bad_rows;  // "line N: reason"
};

inline const Manifest& manifest() {
  static const Manifest m = [] {
    Manifest result;
    std::ifstream in(testdata("manifest.tsv"));
    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
      ++line_no;
      if (line.empty() || line[0] == '#') continue;
      auto cols = split_tabs(line);
      if (cols[0] == "file") continue;
      if (cols.size() < 8) {
        result.bad_rows.push_back("line " + std::to_string(line_no) + ": fewer than 8 columns");
        continue;
      }
      ManifestEntry e;
      e.file = cols[0];
      e.expect = cols[1];
      e.password = cols[2];
      e.owner_password = cols[3];
      if (!parse_number(cols[4], e.pages) || !parse_number(cols[5], e.width) ||
          !parse_number(cols[6], e.height)) {
        result.bad_rows.push_back("line " + std::to_string(line_no) + ": pages/width/height not numeric");
        continue;
      }
      e.render = cols[7] == "1";
      if (cols.size() >= 12 && !cols[11].empty() && !parse_number(cols[11], e.chars)) {
        result.bad_rows.push_back("line " + std::to_string(line_no) + ": chars not numeric");
        continue;
      }
      result.entries.push_back(std::move(e));
    }
    return result;
  }();
  return m;
}

inline std::vector<ManifestEntry> load_manifest() { return manifest().entries; }

// gtest 이름에는 영숫자·밑줄만 쓸 수 있다. 비ASCII 바이트는 hex 로 바꿔 파일마다 고유한 이름을 만든다.
inline std::string test_name(const ::testing::TestParamInfo<ManifestEntry>& info) {
  std::string name;
  for (char c : info.param.file) {
    const auto ch = static_cast<unsigned char>(c);
    if (std::isalnum(ch)) {
      name += static_cast<char>(ch);
    } else if (ch < 0x80) {
      name += '_';
    } else {
      char hex[4];
      std::snprintf(hex, sizeof hex, "x%02X", ch);
      name += hex;
    }
  }
  return name;
}

}  // namespace onpdf::test
