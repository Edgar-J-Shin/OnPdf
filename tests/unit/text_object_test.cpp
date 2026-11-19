// PdfiumDocument 텍스트 객체 API (#45): 폰트 사전·마크·재사용, 제거, GenerateContent 가 원본을 보존
#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/save.h"
#include "onpdf/edit/font_face.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <fstream>
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
  std::vector<std::pair<std::uint32_t, char32_t>> cu;
  for (const auto& g : shaped.glyphs) {
    line.cids.push_back(subset->old_to_new.at(g.gid));
    cu.emplace_back(subset->old_to_new.at(g.gid), g.codepoint);
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
  ASSERT_EQ(ids->size(), 1u);
  EXPECT_EQ(ids->front().rfind("onpdf-text-", 0), 0u);
  EXPECT_EQ(doc->font_cache_size(), 1);
  // 같은 key → 재사용, 주어진 id 유지
  TextObjectsSpec again = spec;
  again.ids = {"onpdf-text-0123456789abcdef"};
  again.lines[0].origin.y += 40;
  auto ids2 = doc->add_text_objects(0, again);
  ASSERT_TRUE(ids2);
  EXPECT_EQ(ids2->front(), "onpdf-text-0123456789abcdef");
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
  for (std::size_t at = bytes.find("/FontFile2"); at != std::string::npos; at = bytes.find("/FontFile2", at + 1)) ++fontfiles;
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
  for (std::size_t i = 0; i < before->pixels.size(); ++i) changed += before->pixels[i] != with_text->pixels[i];
  EXPECT_GT(changed, 1000) << "글이 그려졌다";
  EXPECT_FALSE(doc->remove_text_objects(0, {"onpdf-text-ffffffffffffffff"})) << "없는 id";
  EXPECT_FALSE(doc->remove_text_objects(0, {ids->front(), "onpdf-text-ffffffffffffffff"})) << "하나라도 없으면 전부 거절";
  EXPECT_EQ(doc->text_objects(0)->size(), 1u) << "부분 삭제 없음";
  ASSERT_TRUE(doc->remove_text_objects(0, *ids));
  EXPECT_TRUE(doc->text_objects(0)->empty());
  auto after = doc->render_page(0, options);
  ASSERT_TRUE(after);
  int diff = 0;
  for (std::size_t i = 0; i < before->pixels.size(); ++i) diff += before->pixels[i] != after->pixels[i];
  EXPECT_EQ(diff, 0) << "GenerateContent 가 원본 내용을 바꾸지 않는다";
  // 저장 검증: 저장 전후 텍스트 객체 집합이 같아야 한다 (지운 상태에서도 통과)
  ScopedTempDir dir;
  EXPECT_TRUE(save_document_safely(*doc, dir / "removed.pdf", {}));
}

}  // namespace
}  // namespace onpdf::engine
