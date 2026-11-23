// 시스템 글꼴 카탈로그 (#73): 이름 정규화·매칭·한글 기본·스캔. PDFium 없음
#include "onpdf/edit/font_catalog.h"

#include "../test_support.h"
#include <gtest/gtest.h>

namespace onpdf::edit {
namespace {

using onpdf::test::testdata;

TEST(FontCatalog, NormalizeName) {
  EXPECT_EQ(FontCatalog::normalize_name("ABCDEF+NotoSansKR-Regular"), "notosanskr");
  EXPECT_EQ(FontCatalog::normalize_name("Arial,Bold"), "arial");
  EXPECT_EQ(FontCatalog::normalize_name("ArialMT"), "arial");
  EXPECT_EQ(FontCatalog::normalize_name("Helvetica"), "helvetica");
  EXPECT_EQ(FontCatalog::normalize_name("Apple SD Gothic Neo"), "applesdgothicneo");
  EXPECT_EQ(FontCatalog::normalize_name("맑은 고딕"), "맑은고딕");
  EXPECT_EQ(FontCatalog::normalize_name("Noto Sans CJK KR"), "notosanscjkkr");
  EXPECT_EQ(FontCatalog::normalize_name(""), "");
  EXPECT_EQ(FontCatalog::normalize_name("Times-Roman"), "times");
  EXPECT_EQ(FontCatalog::normalize_name("Courier-BoldOblique"), "courier");
  EXPECT_EQ(FontCatalog::normalize_name("Arial-BoldItalicMT"), "arial");
  EXPECT_EQ(FontCatalog::normalize_name("Kobold"), "kobold") << "하이픈 없는 접미는 둔다";
}

TEST(FontCatalog, ScanCanBeCancelled) {
  int calls = 0;
  const FontCatalog catalog =
      FontCatalog::scan({{testdata("fonts"), false}}, 0, [&] { return ++calls > 1; });
  EXPECT_LE(catalog.entries().size(), 2u) << "첫 파일 뒤에 중단";
  EXPECT_GE(calls, 2);
}

std::vector<FontEntry> fake_entries() {
  return {
      {"/f/Arial Bold.ttf", 0, "Arial", "Bold", false, false, 3000, 700},
      {"/f/Arial.ttf", 0, "Arial", "Regular", false, false, 3000, 400},
      {"/f/ArialCFF.otf", 0, "Arial", "Regular", true, false, 3000, 400},
      {"/f/AppleSDGothicNeo.ttc", 0, "Apple SD Gothic Neo", "Regular", true, true, 18000, 400},
      {"/f/AppleSDGothicNeo.ttc", 6, "Apple SD Gothic Neo", "Bold", true, true, 18000, 700},
      {"/f/AppleGothic.ttf", 0, "AppleGothic", "Regular", false, true, 19000, 400},
  };
}

TEST(FontCatalog, MatchPrefersRegularThenGlyfAndFiltersCff) {
  const FontCatalog catalog = FontCatalog::from_entries(fake_entries());
  auto arial = catalog.match("ABCDEF+ArialMT", true);
  ASSERT_EQ(arial.size(), 3u);
  EXPECT_EQ(arial[0]->path, "/f/Arial.ttf") << "Regular + glyf 우선";
  EXPECT_EQ(arial[1]->path, "/f/ArialCFF.otf");
  EXPECT_EQ(arial[2]->style, "Bold");
  auto arial_no_cff = catalog.match("Arial", false);
  ASSERT_EQ(arial_no_cff.size(), 2u);
  EXPECT_FALSE(arial_no_cff[0]->cff);
  EXPECT_FALSE(arial_no_cff[1]->cff);
  auto bold = catalog.match("Arial,Bold", true);
  ASSERT_FALSE(bold.empty());
  EXPECT_EQ(bold[0]->path, "/f/Arial.ttf") << "',' 뒤 스타일은 버린다 (P2 굵기 매칭은 범위 외)";
  auto joined = catalog.match("ArialBold", true);
  ASSERT_FALSE(joined.empty()) << "family+style 붙은 이름도 family 로";
  EXPECT_TRUE(catalog.match("Helvetica", true).empty());
  EXPECT_TRUE(catalog.match("", true).empty());
}

TEST(FontCatalog, DefaultHangulSansFollowsPreferenceAndCffFlag) {
  const FontCatalog catalog = FontCatalog::from_entries(fake_entries());
  const FontEntry* with_cff = catalog.default_hangul_sans(true);
  ASSERT_NE(with_cff, nullptr);
  EXPECT_EQ(with_cff->family, "Apple SD Gothic Neo");
  EXPECT_EQ(with_cff->face_index, 0u) << "Regular 면";
  const FontEntry* glyf_only = catalog.default_hangul_sans(false);
  ASSERT_NE(glyf_only, nullptr);
  EXPECT_EQ(glyf_only->family, "AppleGothic");
  EXPECT_EQ(FontCatalog::from_entries({}).default_hangul_sans(true), nullptr);
  std::vector<FontEntry> latin_only = {
      {"/f/Arial.ttf", 0, "Arial", "Regular", false, false, 3000, 400}};
  EXPECT_EQ(FontCatalog::from_entries(latin_only).default_hangul_sans(true), nullptr);
}

TEST(FontCatalog, ScansTestdataFonts) {
  const FontCatalog catalog =
      FontCatalog::scan({{testdata("fonts"), false}, {testdata("nonexistent-dir"), false}});
  // ttf(1) + otf(1) + ttc(2 면) = 4; 글리프 ≥ 1,000 조건에 걸리지 않게 테스트 글꼴은 477·484
  // 글리프라… → 최소치 미만이므로 제외된다 저장소 서브셋은 작아서 기본 문턱에 걸린다: 스캔 결과는
  // 비어 있어야 한다 (문턱 검증)
  EXPECT_TRUE(catalog.empty()) << "글리프 < 1,000 은 제외";
}

TEST(FontCatalog, ScanWithoutThresholdFindsAllFaces) {
  const FontCatalog catalog = FontCatalog::scan({{testdata("fonts"), false}}, 0);
  ASSERT_EQ(catalog.entries().size(), 5u);  // ttf·otf(CID)·otf(비CID)·ttc 2면
  int cff = 0, hangul = 0, face1 = 0;
  for (const auto& e : catalog.entries()) {
    if (e.cff) ++cff;
    if (e.hangul) ++hangul;
    if (e.face_index == 1) ++face1;
    EXPECT_EQ(e.style, "Regular") << e.family;
  }
  EXPECT_EQ(cff, 2);
  EXPECT_EQ(hangul, 4) << "SourceSans3 에는 한글이 없다";
  EXPECT_EQ(face1, 1);
  auto face1_match = catalog.match("NotoSansKR-subset-Face1", true);
  ASSERT_EQ(face1_match.size(), 1u);
  EXPECT_EQ(face1_match[0]->face_index, 1u);
  EXPECT_EQ(face1_match[0]->path.filename(), "NotoSansKR-subset.ttc");
  auto cff_match = catalog.match("NotoSansCJKkr-subset", false);
  EXPECT_TRUE(cff_match.empty()) << "CFF 제외";
}

}  // namespace
}  // namespace onpdf::edit
