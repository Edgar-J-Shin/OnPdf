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

TextLayout layout_text(const FontFace& face, std::string_view utf8, double size_pt, Point origin_pt, double line_spacing) {
  TextLayout out;
  const std::vector<std::string> lines = split_lines(utf8);
  const double line_height = line_spacing * size_pt;
  double max_width = 0.0;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const ShapedText shaped = face.shape(lines[i], size_pt);
    if (i == 0) {
      out.ascent = shaped.ascent;
      out.descent = shaped.descent;
    }
    LaidOutLine line;
    line.glyphs = shaped.glyphs;
    line.width = shaped.width;
    line.baseline_origin = {origin_pt.x, origin_pt.y + out.ascent + static_cast<double>(i) * line_height};
    max_width = std::max(max_width, shaped.width);
    out.lines.push_back(std::move(line));
  }
  const double height = lines.empty() ? 0.0 : out.ascent + out.descent + static_cast<double>(lines.size() - 1) * line_height;
  out.bbox = {origin_pt.x, origin_pt.y, max_width, height};
  return out;
}

}  // namespace onpdf::edit
