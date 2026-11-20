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

// 폭 줄바꿈 (#49): 공백 경계, 줄 끝 공백 버림, 긴 단어는 글자 단위, '\n' 은 그대로
TEST(TextLayout, WrapsWithinWidth) {
  auto face = FontFace::load(testdata("fonts") / "NotoSansKR-subset.ttf");
  ASSERT_TRUE(face);
  const double word = (*face)->shape("ABC", 20.0).width;
  const double space = (*face)->shape(" ", 20.0).width;
  // "ABC ABC ABC" 를 두 단어 폭에 → 2줄 ("ABC ABC" / "ABC")
  TextLayout l = layout_text(**face, "ABC ABC ABC", 20.0, {0, 0}, 1.2, 2 * word + space + 0.5);
  ASSERT_EQ(l.lines.size(), 2u);
  EXPECT_EQ(l.lines[0].text, "ABC ABC");
  EXPECT_EQ(l.lines[1].text, "ABC") << "이어진 줄의 앞 공백·줄 끝 공백은 버린다";
  EXPECT_LE(l.lines[0].width, 2 * word + space + 0.5);
  EXPECT_DOUBLE_EQ(l.bbox.w, 2 * word + space + 0.5) << "폭이 정해지면 상자 폭 = 폭";
  // 한 단어가 폭보다 길면 글자 단위
  l = layout_text(**face, "ABCDEFGH", 20.0, {0, 0}, 1.2, word);
  ASSERT_GE(l.lines.size(), 2u);
  for (const auto& line : l.lines) EXPECT_LE(line.width, word + 0.01);
  std::string joined;
  for (const auto& line : l.lines) joined += line.text;
  EXPECT_EQ(joined, "ABCDEFGH") << "글자를 잃지 않는다";
  // 한글 어절 + 개행
  l = layout_text(**face, "한글 텍스트 추가\n둘째", 20.0, {0, 0}, 1.2, (*face)->shape("한글 텍스트", 20.0).width + 1);
  ASSERT_EQ(l.lines.size(), 3u);
  EXPECT_EQ(l.lines[0].text, "한글 텍스트");
  EXPECT_EQ(l.lines[1].text, "추가");
  EXPECT_EQ(l.lines[2].text, "둘째");
  EXPECT_DOUBLE_EQ(l.lines[2].baseline_origin.y, l.ascent + 2 * 1.2 * 20.0);
  // 폭 0 = 줄바꿈 없음
  EXPECT_EQ(layout_text(**face, "ABC ABC ABC", 20.0, {0, 0}).lines.size(), 1u);
}

}  // namespace
}  // namespace onpdf::edit
