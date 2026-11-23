// PdfiumDocument 텍스트 객체 API (#45): 폰트 사전·마크·재사용, 제거, GenerateContent 가 원본을 보존
#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/save.h"
#include "onpdf/edit/font_face.h"

#include "../test_support.h"
#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>

namespace onpdf::engine {
namespace {

using onpdf::test::ScopedTempDir;
using onpdf::test::testdata;

// 테스트 폰트로 TextObjectsSpec 을 만든다 (edit 의 글꼴 서비스 사용)
TextObjectsSpec spec_for(const std::string& text, Point origin, double size, Color color) {
  auto face = edit::FontFace::load(testdata("fonts") / "NotoSansKR-subset.ttf");
  EXPECT_TRUE(face);
  const edit::ShapedText shaped = (*face)->shape(text, size);
  std::vector<std::uint32_t> gids;
  for (const auto& g : shaped.glyphs) gids.push_back(g.gid);
  auto subset = (*face)->subset(gids);
  EXPECT_TRUE(subset);
  TextObjectsSpec spec;
  spec.font.key = subset->key;
  spec.font.ttf = subset->ttf;
  spec.font.cid_to_gid = subset->cid_to_gid;
  TextLineSpec line;
  line.origin = {origin.x, origin.y + shaped.ascent};
  line.font_size = size;
  line.color = color;
  std::vector<std::pair<std::uint32_t, std::u32string>> cu;
  for (const auto& g : shaped.glyphs) {
    line.cids.push_back(subset->old_to_new.at(g.gid));
    cu.emplace_back(subset->old_to_new.at(g.gid), g.text);
  }
  spec.font.to_unicode_cmap = edit::make_to_unicode_cmap(cu);
  spec.lines.push_back(line);
  return spec;
}

std::string file_bytes(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

TEST(TextObjects, AddWritesFontAndMarkAndReusesFont) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  const TextObjectsSpec spec = spec_for("한글 임베딩", {72, 500}, 24.0, {0, 0, 255, 255});
  auto ids = doc->add_text_objects(0, spec);
  ASSERT_TRUE(ids) << ids.error().message;
  ASSERT_EQ(ids->ids.size(), 1u);
  EXPECT_EQ(ids->ids.front().rfind("onpdf-text-", 0), 0u);
  EXPECT_EQ(doc->font_cache_size(), 1);
  // 같은 key → 재사용, 주어진 id 유지
  TextObjectsSpec again = spec;
  again.ids = {"onpdf-text-0123456789abcdef"};
  again.lines[0].origin.y += 40;
  auto ids2 = doc->add_text_objects(0, again);
  ASSERT_TRUE(ids2);
  EXPECT_EQ(ids2->ids.front(), "onpdf-text-0123456789abcdef");
  EXPECT_EQ(doc->font_cache_size(), 1);
  auto list = doc->text_objects(0);
  ASSERT_TRUE(list);
  ASSERT_EQ(list->size(), 2u);
  EXPECT_NEAR((*list)[0].bbox.x, 72.0, 2.0);
  // 저장: 폰트 사전 1개(FontFile2 1개), 마크, ToUnicode
  ScopedTempDir dir;
  const auto saved = dir / "text.pdf";
  ASSERT_TRUE(save_document_safely(*doc, saved, {}));
  const std::string bytes = file_bytes(saved);
  int fontfiles = 0;
  for (std::size_t at = bytes.find("/FontFile2"); at != std::string::npos;
       at = bytes.find("/FontFile2", at + 1))
    ++fontfiles;
  EXPECT_EQ(fontfiles, 1) << "같은 서브셋은 한 번만 임베딩";
  EXPECT_NE(bytes.find("/CIDToGIDMap"), std::string::npos);
  EXPECT_NE(bytes.find("/ToUnicode"), std::string::npos);
  auto reopened = PdfiumDocument::open(saved);
  ASSERT_TRUE(reopened);
  EXPECT_EQ(reopened->text_objects(0)->size(), 2u) << "마크 id 가 저장 뒤에도 남는다";
  EXPECT_NE(reopened->load_text(0)->all_text().find("한글 임베딩"), std::string::npos);
  // 잘못된 입력
  TextObjectsSpec bad = spec;
  bad.lines.clear();
  EXPECT_FALSE(doc->add_text_objects(0, bad));
  bad = spec;
  bad.ids = {"a", "b"};
  EXPECT_FALSE(doc->add_text_objects(0, bad)) << "ids/lines 불일치";
  EXPECT_FALSE(doc->add_text_objects(5, spec));
}

// #47: 마크 box·text 블롭, 크기·색·원점 조회, 옛 객체(box·text 없음) 호환
TEST(TextObjects, ListsBoxTextSizeColorAndOrigin) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  TextObjectsSpec spec = spec_for("한글 원문", {72, 500}, 18.0, {10, 20, 30, 255});
  spec.lines[0].text = "한글 원문";
  spec.box = "onpdf-box-0000000000000001";
  spec.width = 123.4567;  // 폭 마크 (#49): 로캘과 무관한 "123.457"
  auto added = doc->add_text_objects(0, spec);
  ASSERT_TRUE(added);
  EXPECT_EQ(added->box, "onpdf-box-0000000000000001");
  // box 를 주지 않으면 엔진이 만든다
  TextObjectsSpec other = spec_for("둘째", {72, 540}, 12.0, {0, 0, 0, 255});
  other.lines[0].text = "둘째";
  auto added2 = doc->add_text_objects(0, other);
  ASSERT_TRUE(added2);
  EXPECT_EQ(added2->box.rfind("onpdf-box-", 0), 0u);
  EXPECT_NE(added2->box, added->box);
  // 옛 객체 흉내: box·text 마크 없음
  TextObjectsSpec legacy = spec_for("옛 객체", {72, 580}, 12.0, {0, 0, 0, 255});
  legacy.write_box = false;
  legacy.lines[0].text = "옛 객체";
  auto added3 = doc->add_text_objects(0, legacy);
  ASSERT_TRUE(added3);
  auto list = doc->text_objects(0);
  ASSERT_TRUE(list);
  ASSERT_EQ(list->size(), 3u);
  const auto& a = (*list)[0];
  EXPECT_EQ(a.id, added->ids.front());
  EXPECT_EQ(a.box, "onpdf-box-0000000000000001");
  EXPECT_EQ(a.text, "한글 원문") << "원문은 블롭에서";
  EXPECT_DOUBLE_EQ(a.width, 123.457);
  EXPECT_EQ((*list)[1].width, 0.0) << "폭이 없으면 0 (줄바꿈 없음)";
  EXPECT_FLOAT_EQ(static_cast<float>(a.font_size), 18.0f);
  EXPECT_EQ(a.color, (Color{10, 20, 30, 255}));
  EXPECT_NEAR(a.baseline_origin.x, 72.0, 0.01);
  EXPECT_NEAR(a.baseline_origin.y, spec.lines[0].origin.y, 0.01);
  const auto& legacy_info = (*list)[2];
  EXPECT_EQ(legacy_info.box, legacy_info.id) << "box 마크가 없으면 줄 하나가 상자 하나";
  EXPECT_EQ(legacy_info.text, "옛 객체") << "원문 블롭이 없으면 ToUnicode 추출";
  // 저장 → 다시 열어도 마크가 남는다
  ScopedTempDir dir;
  const auto saved = dir / "boxes.pdf";
  ASSERT_TRUE(save_document_safely(*doc, saved, {}));
  auto reopened = PdfiumDocument::open(saved);
  ASSERT_TRUE(reopened);
  auto again = reopened->text_objects(0);
  ASSERT_TRUE(again);
  ASSERT_EQ(again->size(), 3u);
  EXPECT_EQ((*again)[0].box, "onpdf-box-0000000000000001");
  EXPECT_EQ((*again)[0].text, "한글 원문");
  EXPECT_DOUBLE_EQ((*again)[0].width, 123.457);
  EXPECT_NEAR((*again)[0].baseline_origin.y, a.baseline_origin.y, 0.01);
}

// #47: 이동은 쪽 포인트 Δ — 회전 4방향에서 화면 방향으로, 원본 내용 보존
TEST(TextObjects, MoveShiftsInPagePointsOnAnyRotation) {
  for (int rotation = 0; rotation < 4; ++rotation) {
    auto doc = PdfiumDocument::open(testdata("simple.pdf"));
    ASSERT_TRUE(doc);
    if (rotation) ASSERT_TRUE(doc->set_page_rotation(0, rotation));
    RenderOptions options;
    options.scale = 1.0f;
    auto before = doc->render_page(0, options);
    ASSERT_TRUE(before);
    TextObjectsSpec spec =
        spec_for("회전", {100, 100}, 20.0, {0, 0, 0, 255});  // 테스트 폰트에 있는 글자만
    auto added = doc->add_text_objects(0, spec);
    ASSERT_TRUE(added) << "rotation " << rotation;
    const auto origin_before = (*doc->text_objects(0))[0].baseline_origin;
    ASSERT_TRUE(doc->move_page_objects(0, added->ids, {60.0, 40.0})) << "rotation " << rotation;
    const auto after = (*doc->text_objects(0))[0];
    EXPECT_NEAR(after.baseline_origin.x, origin_before.x + 60.0, 0.05) << "rotation " << rotation;
    EXPECT_NEAR(after.baseline_origin.y, origin_before.y + 40.0, 0.05) << "rotation " << rotation;
    // 추출 글자 상자도 같이 움직인다
    bool found = false;
    for (const auto& ch : doc->load_text(0)->chars) {
      if (ch.code == U'회') {
        found = true;
        EXPECT_NEAR(ch.box.x, 160.0, 3.0) << "rotation " << rotation;
      }
    }
    EXPECT_TRUE(found);
    EXPECT_FALSE(doc->move_page_objects(0, {"onpdf-text-ffffffffffffffff"}, {1, 1}));
    // 지우면 원본 그대로
    ASSERT_TRUE(doc->remove_text_objects(0, added->ids));
    auto restored = doc->render_page(0, options);
    ASSERT_TRUE(restored);
    int diff = 0;
    for (std::size_t i = 0; i < before->pixels.size(); ++i)
      diff += before->pixels[i] != restored->pixels[i];
    EXPECT_EQ(diff, 0) << "rotation " << rotation;
  }
}

TEST(TextObjects, RemoveFindsByMarkAndPreservesOriginalContent) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  RenderOptions options;
  options.scale = 2.0f;
  auto before = doc->render_page(0, options);
  ASSERT_TRUE(before);
  const TextObjectsSpec spec = spec_for("지울 글", {72, 500}, 24.0, {0, 0, 0, 255});
  auto ids = doc->add_text_objects(0, spec);
  ASSERT_TRUE(ids);
  auto with_text = doc->render_page(0, options);
  ASSERT_TRUE(with_text);
  int changed = 0;
  for (std::size_t i = 0; i < before->pixels.size(); ++i)
    changed += before->pixels[i] != with_text->pixels[i];
  EXPECT_GT(changed, 1000) << "글이 그려졌다";
  EXPECT_FALSE(doc->remove_text_objects(0, {"onpdf-text-ffffffffffffffff"})) << "없는 id";
  EXPECT_FALSE(doc->remove_text_objects(0, {ids->ids.front(), "onpdf-text-ffffffffffffffff"}))
      << "하나라도 없으면 전부 거절";
  EXPECT_EQ(doc->text_objects(0)->size(), 1u) << "부분 삭제 없음";
  ASSERT_TRUE(doc->remove_text_objects(0, {ids->ids.front(), ids->ids.front()}))
      << "중복 id 는 한 번만 지운다 (리뷰 I11)";
  EXPECT_TRUE(doc->text_objects(0)->empty());
  auto after = doc->render_page(0, options);
  ASSERT_TRUE(after);
  int diff = 0;
  for (std::size_t i = 0; i < before->pixels.size(); ++i)
    diff += before->pixels[i] != after->pixels[i];
  EXPECT_EQ(diff, 0) << "GenerateContent 가 원본 내용을 바꾸지 않는다";
  // 저장 검증: 저장 전후 텍스트 객체 집합이 같아야 한다 (지운 상태에서도 통과)
  ScopedTempDir dir;
  EXPECT_TRUE(save_document_safely(*doc, dir / "removed.pdf", {}));
}

// 미참조 폰트 판정 (#61): 캐시가 비면 false, 쓰이는 동안 false, 지운 뒤 true, 같은 글자 집합을 다시
// 넣으면 false
TEST(TextObjects, HasUnreferencedFonts) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  EXPECT_FALSE(doc->has_unreferenced_fonts()) << "텍스트를 넣은 적 없음";
  auto first = doc->add_text_objects(0, spec_for("한글 텍스트", {72, 100}, 20, {0, 0, 0, 255}));
  ASSERT_TRUE(first);
  EXPECT_FALSE(doc->has_unreferenced_fonts());
  ASSERT_TRUE(doc->remove_text_objects(0, first->ids));
  EXPECT_TRUE(doc->has_unreferenced_fonts()) << "지운 텍스트의 서브셋이 캐시에만 남는다";
  auto again = doc->add_text_objects(0, spec_for("한글 텍스트", {72, 140}, 20, {0, 0, 0, 255}));
  ASSERT_TRUE(again);
  EXPECT_EQ(doc->font_cache_size(), 1) << "같은 서브셋 key 재사용";
  EXPECT_FALSE(doc->has_unreferenced_fonts());
  auto other = doc->add_text_objects(
      1, spec_for("둘째 쪽", {72, 100}, 20, {0, 0, 0, 255}));  // 다른 글자 집합 → 폰트 2개
  ASSERT_TRUE(other);
  EXPECT_EQ(doc->font_cache_size(), 2);
  EXPECT_FALSE(doc->has_unreferenced_fonts()) << "두 쪽에 걸쳐 모두 쓰인다";
  ASSERT_TRUE(doc->remove_text_objects(1, other->ids));
  EXPECT_TRUE(doc->has_unreferenced_fonts());
}

TEST(TextObjects, HasUnreferencedFontsScansLargeDocumentQuickly) {
  auto doc = PdfiumDocument::open(testdata("large-1000.pdf"));
  ASSERT_TRUE(doc);
  auto added = doc->add_text_objects(999, spec_for("한글", {72, 100}, 20, {0, 0, 0, 255}));
  ASSERT_TRUE(added);
  ASSERT_TRUE(doc->remove_text_objects(999, added->ids));
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_TRUE(doc->has_unreferenced_fonts());
  const auto ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  EXPECT_LT(ms, 2000.0) << "1,000쪽을 훑어도 저장 시간을 눈에 띄게 늘리지 않는다";
}
}  // namespace
}  // namespace onpdf::engine
