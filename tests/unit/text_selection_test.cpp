#include "onpdf/core/view/text_selection.h"

#include <gtest/gtest.h>

namespace onpdf::view {
namespace {

PageTextIndex page_with(const std::u32string& text, int page) {
  PageTextIndex idx;
  idx.page = page;
  for (std::size_t i = 0; i < text.size(); ++i) {
    TextChar c;
    c.code = text[i];
    c.box = {static_cast<double>(i) * 10, 0, 10, 12};
    idx.chars.push_back(c);
  }
  idx.lines.push_back({{0, static_cast<int>(text.size())}, {0, 0, text.size() * 10.0, 12}});
  return idx;
}

TEST(TextSelection, EmptyAndNormalization) {
  TextSelection sel;
  EXPECT_TRUE(sel.empty());
  sel.set({1, 5}, {1, 5});
  EXPECT_TRUE(sel.empty());
  sel.set({2, 7}, {1, 3});  // 역방향
  EXPECT_FALSE(sel.empty());
  EXPECT_EQ(sel.start(), (TextPos{1, 3}));
  EXPECT_EQ(sel.end(), (TextPos{2, 7}));
  sel.clear();
  EXPECT_TRUE(sel.empty());
}

TEST(TextSelection, RangeOnPages) {
  TextSelection sel;
  sel.set({1, 3}, {3, 4});
  EXPECT_EQ(sel.range_on(0, 10), (TextRange{}));
  EXPECT_EQ(sel.range_on(1, 10), (TextRange{3, 10}));
  EXPECT_EQ(sel.range_on(2, 10), (TextRange{0, 10}));
  EXPECT_EQ(sel.range_on(3, 10), (TextRange{0, 4}));
  EXPECT_EQ(sel.range_on(4, 10), (TextRange{}));
  EXPECT_EQ(sel.pages(), (std::vector<int>{1, 2, 3}));
  TextSelection single;
  single.set({2, 8}, {2, 2});
  EXPECT_EQ(single.range_on(2, 10), (TextRange{2, 8}));
}

TEST(TextSelection, JoinsPagesWithBlankLineAndSkipsMissing) {
  const auto p0 = page_with(U"first", 0);
  const auto p2 = page_with(U"third", 2);
  TextSelection sel;
  sel.set({0, 2}, {2, 3});
  const std::string text = sel.text([&](int page) -> const PageTextIndex* {
    if (page == 0) return &p0;
    if (page == 2) return &p2;
    return nullptr;  // 1쪽 미로드
  });
  EXPECT_EQ(text, "rst\n\nthi");
}

}  // namespace
}  // namespace onpdf::view
