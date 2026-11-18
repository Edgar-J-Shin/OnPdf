#include "onpdf/core/page_ranges.h"

#include <algorithm>
#include <charconv>
#include <cstdio>

namespace onpdf {
namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

bool parse_int(std::string_view s, int& out) {
  s = trim(s);
  if (s.empty()) return false;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
  return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

std::string padded(int value, int width) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%0*d", width, value);
  return buf;
}

int digits_of(int n) {
  int d = 1;
  while (n >= 10) {
    n /= 10;
    ++d;
  }
  return d;
}

}  // namespace

Result<std::vector<PageRange>> parse_page_ranges(std::string_view text, int page_count) {
  std::vector<PageRange> out;
  if (page_count <= 0) return make_error(ErrorCode::kInvalidArgument, "document has no pages");
  std::size_t start = 0;
  bool any = false;
  while (start <= text.size()) {
    const std::size_t comma = text.find(',', start);
    const std::string_view item = trim(text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
    start = comma == std::string_view::npos ? text.size() + 1 : comma + 1;
    if (item.empty()) {
      if (comma == std::string_view::npos && !any) break;  // 전체가 비었다
      return make_error(ErrorCode::kInvalidArgument, "empty range item");
    }
    any = true;
    PageRange range;
    const std::size_t dash = item.find('-');
    if (dash == std::string_view::npos) {
      if (!parse_int(item, range.first)) return make_error(ErrorCode::kInvalidArgument, "not a page number: " + std::string(item));
      range.last = range.first;
    } else {
      const std::string_view a = trim(item.substr(0, dash));
      const std::string_view b = trim(item.substr(dash + 1));
      if (a.empty() || !parse_int(a, range.first)) return make_error(ErrorCode::kInvalidArgument, "bad range: " + std::string(item));
      if (b.empty()) {
        range.last = page_count;  // "8-" = 끝까지
      } else if (!parse_int(b, range.last)) {
        return make_error(ErrorCode::kInvalidArgument, "bad range: " + std::string(item));
      }
    }
    if (range.first < 1 || range.last < range.first) return make_error(ErrorCode::kInvalidArgument, "bad range: " + std::string(item));
    if (range.last > page_count) {
      return make_error(ErrorCode::kInvalidArgument, "page out of range (document has " + std::to_string(page_count) + " pages): " + std::string(item));
    }
    out.push_back(range);
  }
  if (out.empty()) return make_error(ErrorCode::kInvalidArgument, "no page ranges");
  return out;
}

std::vector<PageRange> ranges_every(int n, int page_count) {
  std::vector<PageRange> out;
  if (n <= 0 || page_count <= 0) return out;
  for (int first = 1; first <= page_count; first += n) out.push_back({first, std::min(page_count, first + n - 1)});
  return out;
}

std::vector<PageRange> ranges_each(int page_count) { return ranges_every(1, page_count); }

std::string output_name(std::string_view pattern, int index, int total, PageRange range, int page_count) {
  std::string name(pattern);
  const int index_width = digits_of(std::max(total, 1));
  const int page_width = std::max(3, digits_of(std::max(page_count, 1)));
  const std::string number = padded(index, index_width);
  const std::string pages = "p" + padded(range.first, page_width) + "-" + padded(range.last, page_width);
  bool replaced = false;
  for (std::size_t pos = name.find("%d"); pos != std::string::npos; pos = name.find("%d", pos + number.size())) {
    name.replace(pos, 2, number);
    replaced = true;
  }
  for (std::size_t pos = name.find("%r"); pos != std::string::npos; pos = name.find("%r", pos + pages.size())) {
    name.replace(pos, 2, pages);
    replaced = true;
  }
  if (!replaced) {
    const std::size_t dot = name.rfind('.');
    const std::string suffix = "-" + number;
    if (dot == std::string::npos || dot == 0) {
      name += suffix;
    } else {
      name.insert(dot, suffix);
    }
  }
  return name;
}

std::string range_text(PageRange range) {
  return range.first == range.last ? std::to_string(range.first) : std::to_string(range.first) + "-" + std::to_string(range.last);
}

}  // namespace onpdf
