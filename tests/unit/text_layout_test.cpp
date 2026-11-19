// 여러 줄 배치 (#45): 줄 분리·기준선·bbox. PDFium 없음
#include "onpdf/edit/text_layout.h"

#include "../test_support.h"

#include <gtest/gtest.h>

namespace onpdf::edit {
namespace {

using onpdf::test::testdata;

TEST(TextLayout, SplitsLines) {
  EXPECT_EQ(split_lines("a\nb"), (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(split_lines("a\r\nb\r\n\r\n"), (std::vector<std::string>{"a", "b"})) << "CR 제거, 끝의 빈 줄 버림";
  EXPECT_EQ(split_lines("a\n\nb"), (std::vector<std::string>{"a", "", "b"})) << "가운데 빈 줄은 유지";
  EXPECT_TRUE(split_lines("").empty());
  EXPECT_TRUE(split_lines("\n\n").empty());
}

TEST(TextLayout, BaselinesAndBox) {
  auto face = FontFace::load(testdata("fonts") / "NotoSansKR-subset.ttf");
  ASSERT_TRUE(face);
  const TextLayout layout = layout_text(**face, "한글\n\n둘째 줄", 20.0, {100, 200});
  ASSERT_EQ(layout.lines.size(), 3u);
  EXPECT_EQ(layout.lines[0].glyphs.size(), 2u);
  EXPECT_TRUE(layout.lines[1].glyphs.empty());
  EXPECT_EQ(layout.lines[2].glyphs.size(), 4u);
  EXPECT_DOUBLE_EQ(layout.lines[0].baseline_origin.x, 100.0);
  EXPECT_DOUBLE_EQ(layout.lines[0].baseline_origin.y, 200.0 + layout.ascent);
  EXPECT_DOUBLE_EQ(layout.lines[2].baseline_origin.y, 200.0 + layout.ascent + 2 * 1.2 * 20.0);
  EXPECT_DOUBLE_EQ(layout.bbox.x, 100.0);
  EXPECT_DOUBLE_EQ(layout.bbox.y, 200.0);
  EXPECT_DOUBLE_EQ(layout.bbox.w, std::max(layout.lines[0].width, layout.lines[2].width));
  EXPECT_DOUBLE_EQ(layout.bbox.h, layout.ascent + layout.descent + 2 * 1.2 * 20.0);
  EXPECT_TRUE(layout_text(**face, "", 20.0, {0, 0}).lines.empty());
}

}  // namespace
}  // namespace onpdf::edit
