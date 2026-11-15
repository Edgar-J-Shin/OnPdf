#include "onpdf/core/view/text_index.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace onpdf::view {

bool is_safe_uri(std::string_view uri) noexcept {
  const auto colon = uri.find(':');
  if (colon == std::string_view::npos || colon == 0) return false;
  for (char c : uri) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) return false;  // 제어 문자
  }
  std::string scheme(uri.substr(0, colon));
  for (char& c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return scheme == "http" || scheme == "https" || scheme == "mailto";
}

bool is_word_separator(char32_t code) noexcept {
  switch (code) {
    case U' ': case U'\t': case U'\r': case U'\n': case 0x00A0: case 0x3000:
    case U',': case U'.': case U';': case U':': case U'!': case U'?':
    case U'(': case U')': case U'[': case U']': case U'{': case U'}':
    case U'"': case U'\'': case U'/': case U'\\': case U'<': case U'>':
    case U'-': case 0x2014: case 0x2013: case 0x00B7: case 0x2022:
    case 0x201C: case 0x201D: case 0x2018: case 0x2019: case 0x3001: case 0x3002:
      return true;
    default:
      return false;
  }
}

void append_utf8(std::string& out, char32_t code) {
  if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) code = 0xFFFD;
  if (code < 0x80) {
    out += static_cast<char>(code);
  } else if (code < 0x800) {
    out += static_cast<char>(0xC0 | (code >> 6));
    out += static_cast<char>(0x80 | (code & 0x3F));
  } else if (code < 0x10000) {
    out += static_cast<char>(0xE0 | (code >> 12));
    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (code & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (code >> 18));
    out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (code & 0x3F));
  }
}

namespace {

Rect expanded(const Rect& r, double by) { return {r.x - by, r.y - by, r.w + 2 * by, r.h + 2 * by}; }

double distance_to_range(double v, double lo, double hi) {
  if (v < lo) return lo - v;
  if (v > hi) return v - hi;
  return 0.0;
}

}  // namespace

std::optional<int> PageTextIndex::hit_test(Point pt, double tolerance) const {
  for (const auto& line : lines) {
    if (line.box.empty() || !expanded(line.box, tolerance).contains(pt)) continue;
    for (int i = line.chars.start; i < line.chars.end; ++i) {
      const auto& c = chars[static_cast<std::size_t>(i)];
      if (!c.box.empty() && expanded(c.box, tolerance).contains(pt)) return i;
    }
  }
  return std::nullopt;
}

int PageTextIndex::nearest_position(Point pt) const {
  if (lines.empty()) return 0;
  // 1. 세로로 걸치는 줄 중 가로 거리 최소. 없으면 세로 거리 최소
  const TextLine* best = nullptr;
  double best_dist = std::numeric_limits<double>::infinity();
  bool best_vertical_hit = false;
  for (const auto& line : lines) {
    if (line.box.empty()) continue;
    const bool vertical_hit = pt.y >= line.box.y && pt.y < line.box.bottom();
    const double dist = vertical_hit ? distance_to_range(pt.x, line.box.x, line.box.right())
                                     : distance_to_range(pt.y, line.box.y, line.box.bottom());
    if ((vertical_hit && !best_vertical_hit) || (vertical_hit == best_vertical_hit && dist < best_dist)) {
      best = &line;
      best_dist = dist;
      best_vertical_hit = vertical_hit;
    }
  }
  if (best == nullptr) return 0;
  // 2. 줄 안에서 가장 가까운 글자 경계. 세로로 긴 줄(회전 쪽·회전 Tm)은 y 축으로 판정하고,
  //    읽기 방향(첫 글자 → 마지막 글자 중심이 증가/감소)에 따라 "앞/뒤" 를 뒤집는다
  const bool vertical = best->box.h > best->box.w;
  auto center_of = [&](const TextChar& c) { return vertical ? c.box.y + c.box.h / 2.0 : c.box.x + c.box.w / 2.0; };
  const double along = vertical ? pt.y : pt.x;
  const TextChar* first_boxed = nullptr;
  const TextChar* last_boxed = nullptr;
  for (int i = best->chars.start; i < best->chars.end; ++i) {
    const auto& c = chars[static_cast<std::size_t>(i)];
    if (c.box.empty()) continue;
    if (first_boxed == nullptr) first_boxed = &c;
    last_boxed = &c;
  }
  if (first_boxed == nullptr) return best->chars.start;
  const bool forward = center_of(*last_boxed) >= center_of(*first_boxed);  // 좌표 증가 방향 = 읽기 방향
  const double line_lo = vertical ? best->box.y : best->box.x;
  const double line_hi = vertical ? best->box.bottom() : best->box.right();
  if (along <= line_lo) return forward ? best->chars.start : best->chars.end;
  if (along >= line_hi) return forward ? best->chars.end : best->chars.start;
  int pos = best->chars.end;
  double nearest = std::numeric_limits<double>::infinity();
  for (int i = best->chars.start; i < best->chars.end; ++i) {
    const auto& c = chars[static_cast<std::size_t>(i)];
    if (c.box.empty()) continue;
    const double center = center_of(c);
    const double d = std::abs(along - center);
    if (d < nearest) {
      nearest = d;
      const bool before = forward ? along < center : along > center;
      pos = before ? i : i + 1;
    }
  }
  return pos;
}

int PageTextIndex::line_of(int ch) const {
  if (ch < 0 || ch >= char_count()) return -1;
  auto it = std::upper_bound(lines.begin(), lines.end(), ch,
                             [](int value, const TextLine& line) { return value < line.chars.start; });
  if (it == lines.begin()) return -1;
  --it;
  const int idx = static_cast<int>(it - lines.begin());
  return ch < it->chars.end ? idx : -1;
}

TextRange PageTextIndex::line_at(int ch) const {
  const int li = line_of(ch);
  return li < 0 ? TextRange{} : lines[static_cast<std::size_t>(li)].chars;
}

TextRange PageTextIndex::word_at(int ch) const {
  const TextRange line = line_at(ch);
  if (line.empty()) return {};
  if (is_word_separator(chars[static_cast<std::size_t>(ch)].code)) return {ch, ch + 1};
  int s = ch;
  while (s > line.start && !is_word_separator(chars[static_cast<std::size_t>(s - 1)].code)) --s;
  int e = ch + 1;
  while (e < line.end && !is_word_separator(chars[static_cast<std::size_t>(e)].code)) ++e;
  return {s, e};
}

std::vector<Rect> PageTextIndex::selection_rects(TextRange range) const {
  std::vector<Rect> out;
  if (range.empty()) return out;
  for (const auto& line : lines) {
    const int s = std::max(range.start, line.chars.start);
    const int e = std::min(range.end, line.chars.end);
    if (e <= s) continue;
    Rect acc;
    bool any = false;
    for (int i = s; i < e; ++i) {
      const auto& c = chars[static_cast<std::size_t>(i)];
      if (c.box.empty()) continue;
      if (!any) {
        acc = c.box;
        any = true;
      } else {
        const double x0 = std::min(acc.x, c.box.x), y0 = std::min(acc.y, c.box.y);
        const double x1 = std::max(acc.right(), c.box.right()), y1 = std::max(acc.bottom(), c.box.bottom());
        acc = {x0, y0, x1 - x0, y1 - y0};
      }
    }
    if (any) out.push_back(acc);
  }
  return out;
}

std::string PageTextIndex::text(TextRange range) const {
  std::string out;
  const int s = std::clamp(range.start, 0, char_count());
  const int e = std::clamp(range.end, 0, char_count());
  int current_line = -1;
  for (int i = s; i < e; ++i) {
    const int li = line_of(i);
    if (current_line >= 0 && li != current_line) out += '\n';
    current_line = li;
    const auto& c = chars[static_cast<std::size_t>(i)];
    append_utf8(out, c.generated ? U' ' : c.code);
  }
  return out;
}

const Link* PageTextIndex::link_at(Point pt) const noexcept {
  for (const auto& link : links) {
    if (link.box.contains(pt)) return &link;
  }
  return nullptr;
}

}  // namespace onpdf::view
