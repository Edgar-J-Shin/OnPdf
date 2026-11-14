#include "onpdf/core/view/text_index.h"

#include <gtest/gtest.h>

namespace onpdf::view {
namespace {

// 3줄짜리 합성 인덱스. 글자 폭 10, 높이 12, 줄 간격 20. 2줄은 "ab cd." 로 공백·구두점 포함, 3줄 생성 공백 포함
PageTextIndex sample() {
  PageTextIndex idx;
  idx.page = 0;
  auto put = [&](char32_t code, double x, double y, bool generated = false) {
    TextChar c;
    c.code = code;
    c.generated = generated;
    c.box = generated ? Rect{} : Rect{x, y, 10, 12};
    c.baseline_y = y + 10;
    c.font_size = 12;
    idx.chars.push_back(c);
  };
  // 줄 0: "Hello" x=0..50, y=0
  for (int i = 0; i < 5; ++i) put(U"Hello"[i], i * 10.0, 0);
  idx.lines.push_back({{0, 5}, {0, 0, 50, 12}});
  // 줄 1: "ab cd." y=20
  const char32_t l1[] = U"ab cd.";
  for (int i = 0; i < 6; ++i) put(l1[i], i * 10.0, 20);
  idx.lines.push_back({{5, 11}, {0, 20, 60, 12}});
  // 줄 2: "x" + 생성 공백 + "y" y=40
  put(U'x', 0, 40);
  put(U' ', 0, 0, true);
  put(U'y', 20, 40);
  idx.lines.push_back({{11, 14}, {0, 40, 30, 12}});
  idx.links.push_back({{100, 100, 50, 20}, LinkKind::kUri, -1, std::nullopt, "https://example.com"});
  return idx;
}

TEST(TextIndex, LineLookup) {
  const auto idx = sample();
  EXPECT_EQ(idx.line_of(0), 0);
  EXPECT_EQ(idx.line_of(4), 0);
  EXPECT_EQ(idx.line_of(5), 1);
  EXPECT_EQ(idx.line_of(13), 2);
  EXPECT_EQ(idx.line_of(14), -1);
  EXPECT_EQ(idx.line_of(-1), -1);
  EXPECT_EQ(idx.line_at(7), (TextRange{5, 11}));
}

TEST(TextIndex, WordAt) {
  const auto idx = sample();
  EXPECT_EQ(idx.word_at(2), (TextRange{0, 5}));   // Hello
  EXPECT_EQ(idx.word_at(5), (TextRange{5, 7}));   // ab
  EXPECT_EQ(idx.word_at(7), (TextRange{7, 8}));   // 공백은 그 글자 하나
  EXPECT_EQ(idx.word_at(9), (TextRange{8, 10}));  // cd (구두점 앞에서 끝)
  EXPECT_EQ(idx.word_at(10), (TextRange{10, 11})); // '.'
}

TEST(TextIndex, HitTestAndNearestPosition) {
  const auto idx = sample();
  EXPECT_EQ(idx.hit_test({15, 6}), 1);
  EXPECT_EQ(idx.hit_test({15, 6 + 50}), std::nullopt);
  EXPECT_EQ(idx.hit_test({51.5, 6}, 2.0), 4);  // 허용 오차 안
  EXPECT_EQ(idx.hit_test({53, 6}, 2.0), std::nullopt);
  // 줄 왼쪽/오른쪽 밖 → 줄 시작/끝
  EXPECT_EQ(idx.nearest_position({-100, 25}), 5);
  EXPECT_EQ(idx.nearest_position({500, 25}), 11);
  // 글자 중심 기준 앞/뒤
  EXPECT_EQ(idx.nearest_position({13, 6}), 1);   // 'e'(10..20) 중심 15 보다 왼쪽 → 1
  EXPECT_EQ(idx.nearest_position({17, 6}), 2);
  // 줄 사이(y=14..20)는 세로 거리 최소 줄. x=3 은 'a'(중심 5) 왼쪽 → 줄 시작
  EXPECT_EQ(idx.nearest_position({3, 19}), 5);
  EXPECT_EQ(idx.nearest_position({5, 19}), 6);  // 정확히 중심이면 뒤
  // 글자 없는 인덱스
  EXPECT_EQ(PageTextIndex{}.nearest_position({1, 1}), 0);
}

TEST(TextIndex, SelectionRectsAndText) {
  const auto idx = sample();
  auto rects = idx.selection_rects({3, 9});  // "lo" + "ab c"
  ASSERT_EQ(rects.size(), 2u);
  EXPECT_EQ(rects[0], (Rect{30, 0, 20, 12}));
  EXPECT_EQ(rects[1], (Rect{0, 20, 40, 12}));
  EXPECT_TRUE(idx.selection_rects({3, 3}).empty());
  EXPECT_EQ(idx.text({3, 9}), "lo\nab c");
  EXPECT_EQ(idx.text({11, 14}), "x y");  // 생성 공백은 ' '
  EXPECT_EQ(idx.all_text(), "Hello\nab cd.\nx y");
  // 생성 공백만 있는 범위의 사각형은 없음
  EXPECT_TRUE(idx.selection_rects({12, 13}).empty());
}

TEST(TextIndex, LinksAndSafeUri) {
  const auto idx = sample();
  ASSERT_NE(idx.link_at({120, 110}), nullptr);
  EXPECT_EQ(idx.link_at({120, 110})->uri, "https://example.com");
  EXPECT_EQ(idx.link_at({10, 10}), nullptr);
  EXPECT_TRUE(is_safe_uri("https://example.com/a?b=c"));
  EXPECT_TRUE(is_safe_uri("HTTP://EXAMPLE.COM"));
  EXPECT_TRUE(is_safe_uri("mailto:a@b.c"));
  EXPECT_FALSE(is_safe_uri("javascript:alert(1)"));
  EXPECT_FALSE(is_safe_uri("file:///etc/passwd"));
  EXPECT_FALSE(is_safe_uri("data:text/html,x"));
  EXPECT_FALSE(is_safe_uri("ftp://x"));
  EXPECT_FALSE(is_safe_uri("no-scheme"));
  EXPECT_FALSE(is_safe_uri("http://x\ny"));
}

TEST(TextIndex, Utf8Encoding) {
  std::string s;
  append_utf8(s, U'A');
  append_utf8(s, U'한');
  append_utf8(s, 0x1F600);
  append_utf8(s, 0xD800);  // 서러게이트 → U+FFFD
  EXPECT_EQ(s, "A한\xF0\x9F\x98\x80\xEF\xBF\xBD");
}

}  // namespace
}  // namespace onpdf::view
