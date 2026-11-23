// 글꼴 서비스(HarfBuzz): 로드·배치·글리프 없음·서브셋·ToUnicode (#45). PDFium 없음
#include "onpdf/edit/font_face.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>

namespace onpdf::edit {
namespace {

using onpdf::test::testdata;

std::shared_ptr<const FontFace> test_font() {
  auto face = FontFace::load(testdata("fonts") / "NotoSansKR-subset.ttf");
  EXPECT_TRUE(face) << (face ? "" : face.error().message);
  return face ? *face : nullptr;
}

TEST(FontFace, LoadsAndReportsMetrics) {
  auto face = test_font();
  ASSERT_NE(face, nullptr);
  EXPECT_EQ(face->upem(), 1000u);
  EXPECT_FALSE(face->has_weight_axis()) << "저장소 서브셋은 정적 인스턴스";
  EXPECT_FALSE(face->family_name().empty());
  EXPECT_FALSE(FontFace::load(testdata("simple.pdf")));
  EXPECT_FALSE(FontFace::load(testdata("missing.ttf")));
}

TEST(FontFace, ShapesKoreanAndLatinWithAdvances) {
  auto face = test_font();
  ASSERT_NE(face, nullptr);
  const ShapedText shaped = face->shape("한글 ABC", 24.0);
  ASSERT_EQ(shaped.glyphs.size(), 6u);
  EXPECT_EQ(shaped.glyphs[0].text, U"한");
  EXPECT_EQ(shaped.glyphs[2].text, U" ");
  EXPECT_EQ(shaped.glyphs[3].text, U"A");
  for (const auto& g : shaped.glyphs) EXPECT_NE(g.gid, 0u) << "모두 글리프가 있다";
  EXPECT_GT(shaped.glyphs[0].x_advance, 15.0);  // 한글 전각 ≈ 24pt
  EXPECT_LT(shaped.glyphs[2].x_advance, shaped.glyphs[0].x_advance);  // 공백은 좁다
  EXPECT_NEAR(shaped.width, shaped.glyphs[0].x_advance + shaped.glyphs[1].x_advance + shaped.glyphs[2].x_advance +
                                shaped.glyphs[3].x_advance + shaped.glyphs[4].x_advance + shaped.glyphs[5].x_advance, 1e-6);
  EXPECT_GT(shaped.ascent, 15.0);
  EXPECT_GT(shaped.descent, 1.0);
  // 공백과 NBSP 는 같은 글리프여도 코드 포인트가 유지된다 (ToUnicode 가 입력 코드를 쓰도록)
  const ShapedText nbsp = face->shape("a\xC2\xA0" "b", 12.0);
  ASSERT_EQ(nbsp.glyphs.size(), 3u);
  EXPECT_EQ(nbsp.glyphs[1].text, U"\u00A0") << "NBSP 코드 유지";
  EXPECT_TRUE(face->shape("", 12.0).glyphs.empty());
  // 탭: 글리프가 없어도 공백 글리프로 (4칸). NBSP 도 공백 글리프 (리뷰 W1)
  const ShapedText tab = face->shape("a\tb", 24.0);
  ASSERT_EQ(tab.glyphs.size(), 3u);
  EXPECT_NE(tab.glyphs[1].gid, 0u);
  EXPECT_EQ(tab.glyphs[1].text, U"\t");
  EXPECT_NEAR(tab.glyphs[1].x_advance, 4.0 * shaped.glyphs[2].x_advance, 1e-6) << "탭 = 공백 ×4";
  for (const auto& g : nbsp.glyphs) EXPECT_NE(g.gid, 0u);
  // NFD 한글(초성·중성·종성) → 글리프 하나, text 에 코드 3개 (리뷰 W3)
  const ShapedText nfd = face->shape("\xE1\x84\x92\xE1\x85\xA1\xE1\x86\xAB", 12.0);  // ᄒ ᅡ ᆫ
  ASSERT_EQ(nfd.glyphs.size(), 1u);
  EXPECT_NE(nfd.glyphs[0].gid, 0u);
  EXPECT_EQ(nfd.glyphs[0].text, U"\u1112\u1161\u11AB");
}

TEST(FontFace, ReportsMissingGlyphs) {
  auto face = test_font();
  ASSERT_NE(face, nullptr);
  EXPECT_TRUE(face->missing("한글 추가 123").empty());
  const std::u32string missing = face->missing("한글 😀 Ωμ 😀");
  ASSERT_EQ(missing.size(), 3u) << "중복 제거, 입력 순서";
  EXPECT_EQ(missing[0], U'😀');
  EXPECT_EQ(missing[1], U'Ω');
  EXPECT_EQ(missing[2], U'μ');
}

TEST(FontFace, SubsetRemapsGlyphsAndShrinks) {
  auto face = test_font();
  ASSERT_NE(face, nullptr);
  const ShapedText shaped = face->shape("한글 텍스트", 24.0);
  std::vector<std::uint32_t> gids;
  for (const auto& g : shaped.glyphs) gids.push_back(g.gid);
  auto subset = face->subset(gids);
  ASSERT_TRUE(subset) << subset.error().message;
  EXPECT_LT(subset->ttf.size(), 20000u) << "6 글리프 서브셋은 작다";
  EXPECT_EQ(subset->glyph_count, 7u) << ".notdef + 6 (공백 포함 6 글자, 중복 없음)";
  EXPECT_EQ(subset->cid_to_gid.size(), subset->glyph_count * 2u);
  EXPECT_EQ(subset->key.size(), 16u);
  std::set<std::uint32_t> new_gids;
  for (std::uint32_t gid : gids) {
    ASSERT_TRUE(subset->old_to_new.contains(gid));
    const std::uint32_t mapped = subset->old_to_new.at(gid);
    EXPECT_GT(mapped, 0u);
    EXPECT_LT(mapped, subset->glyph_count);
    new_gids.insert(mapped);
  }
  EXPECT_EQ(new_gids.size(), 6u);
  // 같은 글리프 집합 → 같은 바이트·같은 key (문서 폰트 캐시의 전제)
  auto again = face->subset(gids);
  ASSERT_TRUE(again);
  EXPECT_EQ(again->key, subset->key);
  // 서브셋을 다시 열어 배치할 수 있다 (정적 TTF)
  auto reloaded = FontFace::load_from_memory(subset->ttf);
  ASSERT_TRUE(reloaded) << reloaded.error().message;
  EXPECT_EQ((*reloaded)->upem(), 1000u);
}

TEST(FontFace, ToUnicodeCmapFormat) {
  const std::string cmap = make_to_unicode_cmap({{3, U"글"}, {1, U"한"}, {1, U"\u00A0"}, {5, U"\U0001F600"}, {7, U"\u1112\u1161\u11AB"}});
  EXPECT_NE(cmap.find("begincodespacerange\n<0000> <FFFF>"), std::string::npos);
  EXPECT_NE(cmap.find("4 beginbfchar\n<0001> <D55C>\n<0003> <AE00>\n<0005> <D83DDE00>\n<0007> <1112116111AB>\nendbfchar"),
            std::string::npos)
      << cmap;  // 정렬, 중복 CID 는 먼저 나온 문자열, 서로게이트 쌍, 다중 코드 dst
  EXPECT_NE(cmap.find("/CMapName /OnPdf-Identity def"), std::string::npos);
}

TEST(FontFace, DecodesUtf8) {
  const std::u32string s = decode_utf8("a한\xF0\x9F\x98\x80\xFF");
  ASSERT_EQ(s.size(), 4u);
  EXPECT_EQ(s[0], U'a');
  EXPECT_EQ(s[1], U'한');
  EXPECT_EQ(s[2], U'😀');
  EXPECT_EQ(s[3], U'�');
}


// #73: TTC 면 선택·CFF 판별·CFF 서브셋이 OTTO
TEST(FontFace, LoadsTtcFaceByIndexAndDetectsCff) {
  const auto ttc = testdata("fonts") / "NotoSansKR-subset.ttc";
  auto face0 = FontFace::load(ttc);
  ASSERT_TRUE(face0) << face0.error().message;
  EXPECT_EQ((*face0)->family_name(), "NotoSansKR-subset");
  EXPECT_EQ((*face0)->face_index(), 0u);
  EXPECT_FALSE((*face0)->is_cff());
  auto face1 = FontFace::load(ttc, 1);
  ASSERT_TRUE(face1) << face1.error().message;
  EXPECT_EQ((*face1)->family_name(), "NotoSansKR-subset-Face1");
  EXPECT_EQ((*face1)->face_index(), 1u);
  EXPECT_EQ((*face1)->style_name(), "Regular");
  auto out_of_range = FontFace::load(ttc, 2);
  ASSERT_FALSE(out_of_range);
  EXPECT_EQ(out_of_range.error().code, ErrorCode::kInvalidFormat);
  EXPECT_FALSE(FontFace::load(testdata("fonts") / "NotoSansKR-subset.ttf", 1)) << "단일 글꼴은 면 0 만";
  // 면 1 의 서브셋은 그 면의 이름을 가진 독립 글꼴이다
  const auto shaped = (*face1)->shape("한글", 12.0);
  std::vector<std::uint32_t> gids;
  for (const auto& g : shaped.glyphs) gids.push_back(g.gid);
  auto subset = (*face1)->subset(gids);
  ASSERT_TRUE(subset) << subset.error().message;
  auto reloaded = FontFace::load_from_memory(subset->ttf);
  ASSERT_TRUE(reloaded);
  EXPECT_EQ((*reloaded)->family_name(), "NotoSansKR-subset-Face1");
  EXPECT_FALSE((*reloaded)->is_cff());

  auto cff = FontFace::load(testdata("fonts") / "NotoSansCJKkr-subset.otf");
  ASSERT_TRUE(cff) << cff.error().message;
  EXPECT_TRUE((*cff)->is_cff());
  EXPECT_EQ((*cff)->family_name(), "NotoSansCJKkr-subset");
  EXPECT_TRUE((*cff)->missing("한글 텍스트 123").empty());
  const auto cff_shaped = (*cff)->shape("한글", 12.0);
  gids.clear();
  for (const auto& g : cff_shaped.glyphs) gids.push_back(g.gid);
  auto cff_subset = (*cff)->subset(gids);
  ASSERT_TRUE(cff_subset) << cff_subset.error().message;
  ASSERT_GE(cff_subset->ttf.size(), 4u);
  EXPECT_EQ(std::string(cff_subset->ttf.begin(), cff_subset->ttf.begin() + 4), "OTTO") << "CFF 서브셋은 OpenType(OTTO)";
  EXPECT_NE(cff_subset->key, subset->key);
}
}  // namespace
}  // namespace onpdf::edit
