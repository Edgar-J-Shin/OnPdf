#include "onpdf/edit/text_layout.h"

#include <algorithm>

namespace onpdf::edit {

std::vector<std::string> split_lines(std::string_view utf8) {
  std::vector<std::string> lines;
  std::string current;
  for (char c : utf8) {
    if (c == '\r') continue;
    if (c == '\n') {
      lines.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(c);
  }
  lines.push_back(current);
  while (!lines.empty() && lines.back().empty()) lines.pop_back();
  return lines;
}

std::string encode_utf8(std::u32string_view text) {
  std::string out;
  for (char32_t c : text) {
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (c >> 6)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (c >> 12)));
      out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (c >> 18)));
      out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
  }
  return out;
}

namespace {

bool is_space_glyph(const ShapedGlyph& g) {
  return !g.text.empty() && std::all_of(g.text.begin(), g.text.end(), [](char32_t c) { return c == U' ' || c == U'\t' || c == 0xA0; });
}

// 한 원문 줄의 글리프를 폭 안의 조각들로 나눈다
std::vector<std::vector<ShapedGlyph>> wrap_glyphs(const std::vector<ShapedGlyph>& glyphs, double max_width) {
  std::vector<std::vector<ShapedGlyph>> out;
  if (max_width <= 0.0 || glyphs.empty()) {
    out.push_back(glyphs);
    return out;
  }
  std::size_t start = 0;
  while (start < glyphs.size()) {
    while (start < glyphs.size() && is_space_glyph(glyphs[start]) && !out.empty()) ++start;  // 이어진 줄의 앞 공백은 버린다
    if (start >= glyphs.size()) break;
    double width = 0.0;
    std::size_t end = start;
    std::size_t last_space = std::string::npos;
    while (end < glyphs.size()) {
      if (is_space_glyph(glyphs[end])) last_space = end;
      if (width + glyphs[end].x_advance > max_width && end > start && !is_space_glyph(glyphs[end])) break;
      width += glyphs[end].x_advance;
      ++end;
    }
    if (end < glyphs.size() && last_space != std::string::npos && last_space > start) end = last_space;  // 공백 경계에서
    std::vector<ShapedGlyph> piece(glyphs.begin() + static_cast<std::ptrdiff_t>(start), glyphs.begin() + static_cast<std::ptrdiff_t>(end));
    while (!piece.empty() && is_space_glyph(piece.back())) piece.pop_back();  // 줄 끝 공백은 버린다
    out.push_back(std::move(piece));
    start = end;
  }
  if (out.empty()) out.emplace_back();
  return out;
}

}  // namespace

TextLayout layout_text(const FontFace& face, std::string_view utf8, double size_pt, Point origin_pt, double line_spacing,
                       double max_width) {
  TextLayout out;
  const std::vector<std::string> lines = split_lines(utf8);
  const double line_height = line_spacing * size_pt;
  double widest = 0.0;
  const ShapedText metrics = face.shape("", size_pt);
  out.ascent = metrics.ascent;
  out.descent = metrics.descent;
  std::size_t row = 0;
  for (const std::string& source : lines) {
    const ShapedText shaped = face.shape(source, size_pt);
    for (auto& glyphs : wrap_glyphs(shaped.glyphs, max_width)) {
      LaidOutLine line;
      for (const auto& g : glyphs) {
        line.width += g.x_advance;
        line.text += encode_utf8(g.text);
      }
      line.glyphs = std::move(glyphs);
      line.baseline_origin = {origin_pt.x, origin_pt.y + out.ascent + static_cast<double>(row) * line_height};
      widest = std::max(widest, line.width);
      out.lines.push_back(std::move(line));
      ++row;
    }
  }
  const double height = out.lines.empty() ? 0.0 : out.ascent + out.descent + static_cast<double>(out.lines.size() - 1) * line_height;
  out.bbox = {origin_pt.x, origin_pt.y, max_width > 0.0 ? max_width : widest, height};
  return out;
}

}  // namespace onpdf::edit
