#pragma once

// 여러 줄 텍스트 배치 (PDFium 없음). 줄 = '\n' 분리, 기준선 = origin.y + ascent + i × line_spacing × size.
// 자동 줄바꿈 없음(P1). 좌표는 쪽 포인트

#include "onpdf/core/geometry.h"
#include "onpdf/edit/font_face.h"

#include <string>
#include <string_view>
#include <vector>

namespace onpdf::edit {

struct LaidOutLine {
  std::vector<ShapedGlyph> glyphs;  // 비어 있으면 빈 줄 (자리만 차지)
  Point baseline_origin;            // 기준선 왼쪽 끝 (쪽 포인트)
  double width = 0.0;
};

struct TextLayout {
  std::vector<LaidOutLine> lines;
  Rect bbox;            // 전체 (쪽 포인트)
  double ascent = 0.0;  // pt
  double descent = 0.0;
};

[[nodiscard]] TextLayout layout_text(const FontFace& face, std::string_view utf8, double size_pt, Point origin_pt,
                                     double line_spacing = 1.2);

// '\n' 으로 나누고 '\r' 을 버린다. 끝의 빈 줄은 유지하지 않는다
[[nodiscard]] std::vector<std::string> split_lines(std::string_view utf8);

}  // namespace onpdf::edit
