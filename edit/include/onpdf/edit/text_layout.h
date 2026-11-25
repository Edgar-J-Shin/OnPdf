#pragma once

// 여러 줄 텍스트 배치 (PDFium 없음). 줄 = '\n' 분리, 기준선 = origin.y + ascent + i × line_spacing
// × size. 자동 줄바꿈 없음(P1). 좌표는 쪽 포인트

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
  std::string text;  // 이 줄의 원문 (줄바꿈으로 나뉜 조각, UTF-8)
};

struct TextLayout {
  std::vector<LaidOutLine> lines;
  Rect bbox;            // 전체 (쪽 포인트)
  double ascent = 0.0;  // pt
  double descent = 0.0;
};

// max_width > 0 이면 그 폭 안에서 줄을 나눈다 (#49): 공백 경계 우선(줄 끝 공백은 버림), 한 단어가
// 넘치면 글자 단위
[[nodiscard]] TextLayout layout_text(const FontFace& face, std::string_view utf8, double size_pt,
                                     Point origin_pt, double line_spacing = 1.2,
                                     double max_width = 0.0);

// 코드 포인트 → UTF-8
[[nodiscard]] std::string encode_utf8(std::u32string_view text);

// '\n' 으로 나누고 '\r' 을 버린다. 끝의 빈 줄은 유지하지 않는다
[[nodiscard]] std::vector<std::string> split_lines(std::string_view utf8);

}  // namespace onpdf::edit
