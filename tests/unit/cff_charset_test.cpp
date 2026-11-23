// CFF charset 검사·항등 덮어쓰기 (#73, core 내부). 서브셋 생성은 edit 의 HarfBuzz 경로를 쓴다
#include "cff_charset.h"

#include "onpdf/edit/font_face.h"

#include "../test_support.h"
#include "onpdf/core/fnv.h"
#include <gtest/gtest.h>

namespace onpdf::engine::detail {
namespace {

using onpdf::test::read_bytes;
using onpdf::test::testdata;

std::vector<std::uint8_t> cff_subset_bytes(const std::string& text, std::string* key = nullptr) {
  auto face = edit::FontFace::load(testdata("fonts") / "NotoSansCJKkr-subset.otf");
  EXPECT_TRUE(face);
  const auto shaped = (*face)->shape(text, 12.0);
  std::vector<std::uint32_t> gids;
  for (const auto& g : shaped.glyphs) gids.push_back(g.gid);
  auto subset = (*face)->subset(gids);
  EXPECT_TRUE(subset);
  if (key != nullptr) *key = subset->key;
  return subset->ttf;
}

TEST(CffCharset, InspectsSubsetAndRewritesIdentity) {
  std::vector<std::uint8_t> otf = cff_subset_bytes("한글 텍스트 123");
  auto info = inspect_cff_charset(otf);
  ASSERT_TRUE(info);
  EXPECT_TRUE(info->cid_keyed);
  EXPECT_FALSE(info->identity) << "hb-subset 은 원래 CID 를 남긴다";
  EXPECT_GT(info->glyphs, 5u);
  EXPECT_GE(info->charset_len, 4u);
  ASSERT_TRUE(write_identity_charset(otf));
  auto after = inspect_cff_charset(otf);
  ASSERT_TRUE(after);
  EXPECT_TRUE(after->identity);
  EXPECT_EQ(after->glyphs, info->glyphs);
  EXPECT_EQ(after->charset_offset, info->charset_offset);
  EXPECT_LE(after->charset_len, info->charset_len);
  EXPECT_TRUE(write_identity_charset(otf)) << "이미 항등이면 true";
  // 큰 글자 집합(> 257 글리프는 아니지만 형식 2 경계는 아래 합성으로)
}

TEST(CffCharset, FullFontCharsetUsesFormat2WhenMoreThan257Glyphs) {
  // 저장소 CFF 글꼴 전체(484 글리프)는 charset 형식 1 — 항등은 형식 2(5B)가 필요하다
  std::vector<std::uint8_t> otf = read_bytes(testdata("fonts") / "NotoSansCJKkr-subset.otf");
  auto info = inspect_cff_charset(otf);
  ASSERT_TRUE(info);
  EXPECT_EQ(info->glyphs, 484u);
  EXPECT_FALSE(info->identity);
  ASSERT_TRUE(write_identity_charset(otf));
  auto after = inspect_cff_charset(otf);
  ASSERT_TRUE(after);
  EXPECT_TRUE(after->identity);
  EXPECT_EQ(after->charset_len, 5u);
}

TEST(CffCharset, RejectsGlyfAndTruncatedData) {
  EXPECT_FALSE(inspect_cff_charset(read_bytes(testdata("fonts") / "NotoSansKR-subset.ttf")));
  std::vector<std::uint8_t> otf = cff_subset_bytes("가");
  for (const std::size_t cut :
       {std::size_t{3}, std::size_t{12}, std::size_t{200}, otf.size() / 2}) {
    std::vector<std::uint8_t> truncated(otf.begin(),
                                        otf.begin() + static_cast<std::ptrdiff_t>(cut));
    EXPECT_FALSE(inspect_cff_charset(truncated)) << cut;
    EXPECT_FALSE(write_identity_charset(truncated)) << cut;
  }
  std::vector<std::uint8_t> glyf = read_bytes(testdata("fonts") / "NotoSansKR-subset.ttf");
  EXPECT_FALSE(write_identity_charset(glyf));
}

TEST(CffCharset, SubsetKeyMatchesCoreFnv) {
  std::string key;
  const auto otf = cff_subset_bytes("키 검증", &key);
  EXPECT_EQ(onpdf::fnv1a_hex(otf), key) << "저장 후처리가 FontFile2 바이트로 OnPdf 글꼴을 찾는 근거";
}


TEST(CffCharset, NonCidFontIsReportedAndLeftAlone) {
  auto face = edit::FontFace::load(testdata("fonts") / "SourceSans3-subset.otf");
  ASSERT_TRUE(face);
  const auto shaped = (*face)->shape("Latin text", 12.0);
  std::vector<std::uint32_t> gids;
  for (const auto& g : shaped.glyphs) gids.push_back(g.gid);
  auto subset = (*face)->subset(gids);
  ASSERT_TRUE(subset);
  std::vector<std::uint8_t> otf = subset->ttf;
  auto info = inspect_cff_charset(otf);
  ASSERT_TRUE(info);
  EXPECT_FALSE(info->cid_keyed);
  const auto copy = otf;
  EXPECT_FALSE(write_identity_charset(otf)) << "비CID 는 손대지 않는다";
  EXPECT_EQ(otf, copy);
}
}  // namespace
}  // namespace onpdf::engine::detail
