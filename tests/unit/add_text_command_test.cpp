// AddTextCommand: 실행 → 취소 → 재실행 (가짜 + PDFium), 회전 쪽, 폰트 재사용 (#45)
#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/edit/add_text_command.h"

#include "../fake_document.h"
#include "../test_support.h"

#include <gtest/gtest.h>

namespace onpdf::edit {
namespace {

using onpdf::test::FakeDocument;
using onpdf::test::testdata;

std::shared_ptr<const FontFace> test_font() {
  auto face = FontFace::load(testdata("fonts") / "NotoSansKR-subset.ttf");
  EXPECT_TRUE(face);
  return face ? *face : nullptr;
}

TextBoxSpec box(int page, const std::string& text, Point origin = {72, 100}, double size = 24.0) {
  TextBoxSpec s;
  s.page = page;
  s.origin = origin;
  s.size = size;
  s.color = {200, 0, 0, 255};
  s.utf8 = text;
  s.face = test_font();
  return s;
}

TEST(AddTextCommand, ExecuteUndoRedoOnFake) {
  FakeDocument doc({{612, 792}});
  AddTextCommand cmd(box(0, "한글 텍스트\n둘째 줄"));
  EXPECT_EQ(cmd.id(), "add_text");
  EXPECT_FALSE(cmd.undo(doc)) << "실행 전 취소는 실패";
  auto change = cmd.execute(doc);
  ASSERT_TRUE(change) << change.error().message;
  EXPECT_EQ(change->pages, std::vector<int>{0});
  ASSERT_EQ(doc.text_objects_on(0).size(), 2u);
  const auto ids = cmd.object_ids();
  ASSERT_EQ(ids.size(), 2u);
  EXPECT_EQ(doc.text_objects_on(0)[0].id, ids[0]);
  EXPECT_EQ(doc.text_objects_on(0)[0].line.cids.size(), 6u);
  EXPECT_EQ(doc.text_objects_on(0)[1].line.cids.size(), 4u);
  EXPECT_DOUBLE_EQ(doc.text_objects_on(0)[0].line.origin.x, 72.0);
  EXPECT_GT(doc.text_objects_on(0)[1].line.origin.y, doc.text_objects_on(0)[0].line.origin.y);
  EXPECT_EQ(doc.text_objects_on(0)[0].line.color, (engine::Color{200, 0, 0, 255}));
  EXPECT_EQ(doc.font_count(), 1) << "두 줄이 같은 폰트";
  ASSERT_TRUE(cmd.undo(doc));
  EXPECT_TRUE(doc.text_objects_on(0).empty());
  ASSERT_TRUE(cmd.execute(doc));
  ASSERT_EQ(doc.text_objects_on(0).size(), 2u);
  EXPECT_EQ(doc.text_objects_on(0)[0].id, ids[0]) << "재실행은 같은 id";
  EXPECT_EQ(doc.text_objects_on(0)[1].id, ids[1]);
  EXPECT_EQ(doc.font_count(), 1) << "같은 서브셋 key 재사용";
}

TEST(AddTextCommand, RejectsEmptyAndPropagatesFailure) {
  FakeDocument doc({{612, 792}});
  AddTextCommand blank(box(0, "   \n\n"));
  EXPECT_FALSE(blank.execute(doc)) << "공백만";
  EXPECT_TRUE(doc.text_objects_on(0).empty());
  TextBoxSpec no_font = box(0, "x");
  no_font.face = nullptr;
  EXPECT_FALSE(AddTextCommand(no_font).execute(doc));
  AddTextCommand tab(box(0, "a\tb"));
  EXPECT_TRUE(tab.execute(doc)) << "탭은 공백으로 (리뷰 W1)";
  const std::size_t before_emoji = doc.text_objects_on(0).size();
  AddTextCommand emoji(box(0, "한글 😀"));
  EXPECT_FALSE(emoji.execute(doc)) << "글꼴에 없는 글자(.notdef)는 거절";
  EXPECT_EQ(doc.text_objects_on(0).size(), before_emoji);
  doc.fail_add_text_on_page = 0;
  AddTextCommand failing(box(0, "한글"));
  EXPECT_FALSE(failing.execute(doc));
  EXPECT_FALSE(failing.undo(doc)) << "실행되지 않았으면 취소도 실패";
}

// 실제 PDFium: 객체 수·추출 텍스트·마크 id, 실행 → 취소 → 재실행
TEST(AddTextCommand, ExecuteUndoRedoOnPdfium) {
  auto doc = engine::PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  const std::string text = "한글 텍스트 추가\nOnPdf 2026";
  AddTextCommand cmd(box(0, text, {72, 300}));
  ASSERT_TRUE(cmd.execute(*doc));
  auto objects = doc->text_objects(0);
  ASSERT_TRUE(objects);
  ASSERT_EQ(objects->size(), 2u);
  EXPECT_EQ((*objects)[0].id, cmd.object_ids()[0]);
  EXPECT_NEAR((*objects)[0].bbox.x, 72.0, 2.0);
  EXPECT_GT((*objects)[0].bbox.y, 290.0);
  EXPECT_LT((*objects)[0].bbox.y, 320.0);
  auto index = doc->load_text(0);
  ASSERT_TRUE(index);
  EXPECT_NE(index->all_text().find("한글 텍스트 추가"), std::string::npos) << index->all_text();
  EXPECT_NE(index->all_text().find("OnPdf 2026"), std::string::npos);
  EXPECT_EQ(doc->font_cache_size(), 1);
  ASSERT_TRUE(cmd.undo(*doc));
  EXPECT_TRUE(doc->text_objects(0)->empty());
  EXPECT_EQ(doc->load_text(0)->all_text().find("한글 텍스트 추가"), std::string::npos);
  ASSERT_TRUE(cmd.execute(*doc));
  ASSERT_EQ(doc->text_objects(0)->size(), 2u);
  EXPECT_EQ((*doc->text_objects(0))[0].id, cmd.object_ids()[0]) << "재실행은 같은 id";
  EXPECT_EQ(doc->font_cache_size(), 1) << "같은 서브셋은 폰트 객체 재사용";
  // NFD 입력(초성·중성·종성)도 추출은 입력 그대로 (ToUnicode 다중 코드, 리뷰 W3)
  AddTextCommand nfd(box(0, "\xE1\x84\x92\xE1\x85\xA1\xE1\x86\xAB" " x", {72, 500}));
  ASSERT_TRUE(nfd.execute(*doc));
  EXPECT_NE(doc->load_text(0)->all_text().find("\xE1\x84\x92\xE1\x85\xA1\xE1\x86\xAB"), std::string::npos) << doc->load_text(0)->all_text();
  ASSERT_TRUE(nfd.undo(*doc));
  // 다른 글자 집합 → 새 폰트, 같은 집합 → 재사용
  const int cached = doc->font_cache_size();  // NFD 상자의 폰트도 캐시에 남는다 (취소해도 사전은 유지)
  AddTextCommand other(box(0, "둘째 줄", {72, 400}));
  ASSERT_TRUE(other.execute(*doc));
  EXPECT_EQ(doc->font_cache_size(), cached + 1);
  AddTextCommand same(box(0, "둘째 줄", {72, 450}));
  ASSERT_TRUE(same.execute(*doc));
  EXPECT_EQ(doc->font_cache_size(), cached + 1);
}

// 암호(소유자) 문서에서도 추가·추출된다
TEST(AddTextCommand, WorksOnOwnerEncryptedDocument) {
  auto doc = engine::PdfiumDocument::open(testdata("perm-modify-only.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  AddTextCommand cmd(box(0, "한글 텍스트", {72, 300}));
  auto change = cmd.execute(*doc);
  ASSERT_TRUE(change) << change.error().message;
  auto index = doc->load_text(0);
  ASSERT_TRUE(index);
  EXPECT_NE(index->all_text().find("한글 텍스트"), std::string::npos) << index->all_text();
}

// 회전 쪽: 쪽 포인트로 넣은 글이 회전 0 과 같은 자리·같은 크기로 추출된다 (화면 기준 바로 선 글)
TEST(AddTextCommand, RotatedPageKeepsPagePointPlacement) {
  Rect reference{};
  for (int rotation = 0; rotation <= 3; ++rotation) {
    auto doc = engine::PdfiumDocument::open(testdata("simple.pdf"));
    ASSERT_TRUE(doc);
    if (rotation != 0) ASSERT_TRUE(doc->set_page_rotation(0, rotation));
    AddTextCommand cmd(box(0, "회전 쪽", {100, 150}, 20.0));
    ASSERT_TRUE(cmd.execute(*doc)) << "rotation " << rotation;
    auto index = doc->load_text(0);
    ASSERT_TRUE(index);
    bool found = false;
    for (const auto& ch : index->chars) {
      if (ch.code != U'회') continue;
      found = true;
      if (rotation == 0) {
        reference = ch.box;
        EXPECT_NEAR(ch.box.x, 100.0, 3.0);
        EXPECT_NEAR(ch.box.y, 150.0, 10.0) << "글자 상자 위 ≈ origin (글리프 bbox 기준)";
        EXPECT_GT(ch.box.w, 10.0);
      } else {
        EXPECT_NEAR(ch.box.x, reference.x, 1.0) << "rotation " << rotation;
        EXPECT_NEAR(ch.box.y, reference.y, 1.0) << "rotation " << rotation;
        EXPECT_NEAR(ch.box.w, reference.w, 1.0) << "rotation " << rotation;
        EXPECT_NEAR(ch.box.h, reference.h, 1.0) << "rotation " << rotation;
      }
    }
    EXPECT_TRUE(found) << "rotation " << rotation;
    ASSERT_TRUE(cmd.undo(*doc));
    EXPECT_TRUE(doc->text_objects(0)->empty());
  }
}

}  // namespace
}  // namespace onpdf::edit
