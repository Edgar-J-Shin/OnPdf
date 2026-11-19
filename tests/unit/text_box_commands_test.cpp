// 텍스트 상자 Command (#47): 이동·삭제·수정 실행 → 취소 → 재실행 (가짜 + PDFium), id·상자·폰트 캐시 재사용, 보상
#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/edit/add_text_command.h"
#include "onpdf/edit/text_box_commands.h"

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

TEST(TextBoxCommands, MoveExecuteUndoRedoOnFake) {
  FakeDocument doc({{612, 792}});
  AddTextCommand add(box(0, "한글\n둘째"));
  ASSERT_TRUE(add.execute(doc));
  const auto ids = add.object_ids();
  const double y0 = doc.text_objects_on(0)[0].line.origin.y;
  MoveTextBoxCommand move(0, ids, {10, 20});
  EXPECT_EQ(move.id(), "move_text");
  EXPECT_FALSE(move.undo(doc));
  ASSERT_TRUE(move.execute(doc));
  EXPECT_DOUBLE_EQ(doc.text_objects_on(0)[0].line.origin.x, 82.0);
  EXPECT_DOUBLE_EQ(doc.text_objects_on(0)[0].line.origin.y, y0 + 20.0);
  EXPECT_DOUBLE_EQ(doc.text_objects_on(0)[1].line.origin.x, 82.0);
  ASSERT_TRUE(move.undo(doc));
  EXPECT_DOUBLE_EQ(doc.text_objects_on(0)[0].line.origin.x, 72.0);
  EXPECT_DOUBLE_EQ(doc.text_objects_on(0)[0].line.origin.y, y0);
  ASSERT_TRUE(move.execute(doc));
  EXPECT_DOUBLE_EQ(doc.text_objects_on(0)[0].line.origin.x, 82.0);
  MoveTextBoxCommand missing(0, {"nope"}, {1, 1});
  EXPECT_FALSE(missing.execute(doc));
}

TEST(TextBoxCommands, RemoveExecuteUndoRedoOnFake) {
  FakeDocument doc({{612, 792}});
  AddTextCommand add(box(0, "한글\n둘째"));
  ASSERT_TRUE(add.execute(doc));
  const auto ids = add.object_ids();
  const std::string box_id = add.box_id();
  RemoveTextBoxCommand remove(0, box_id, test_font());
  EXPECT_EQ(remove.id(), "remove_text");
  ASSERT_TRUE(remove.execute(doc));
  EXPECT_TRUE(doc.text_objects_on(0).empty());
  EXPECT_EQ(remove.content().ids, ids);
  EXPECT_EQ(remove.content().spec.utf8, "한글\n둘째");
  EXPECT_DOUBLE_EQ(remove.content().spec.size, 24.0);
  EXPECT_EQ(remove.content().spec.color, (engine::Color{200, 0, 0, 255}));
  EXPECT_DOUBLE_EQ(remove.content().spec.origin.x, 72.0);
  EXPECT_DOUBLE_EQ(remove.content().spec.origin.y, 100.0) << "원점 = 기준선 − ascent";
  ASSERT_TRUE(remove.undo(doc));
  ASSERT_EQ(doc.text_objects_on(0).size(), 2u);
  EXPECT_EQ(doc.text_objects_on(0)[0].id, ids[0]) << "같은 줄 id";
  EXPECT_EQ(doc.text_objects_on(0)[1].id, ids[1]);
  EXPECT_EQ(doc.text_objects_on(0)[0].box, box_id) << "같은 상자 id";
  EXPECT_EQ(doc.text_objects_on(0)[0].line.text, "한글");
  EXPECT_EQ(doc.font_count(), 1) << "같은 글자 집합 → 같은 폰트 key";
  ASSERT_TRUE(remove.execute(doc));
  EXPECT_TRUE(doc.text_objects_on(0).empty());
  RemoveTextBoxCommand missing(0, "nope", test_font());
  EXPECT_FALSE(missing.execute(doc));
}

TEST(TextBoxCommands, ReplaceExecuteUndoRedoOnFake) {
  FakeDocument doc({{612, 792}});
  AddTextCommand add(box(0, "한글\n둘째"));
  ASSERT_TRUE(add.execute(doc));
  const auto ids = add.object_ids();
  // 내용: 2줄 → 3줄 (id 2개 재사용 + 새 id)
  ReplaceTextBoxCommand replace(0, add.box_id(), test_font(), std::string("첫째\n둘째\n셋째"), std::nullopt, std::nullopt);
  EXPECT_EQ(replace.id(), "replace_text");
  ASSERT_TRUE(replace.execute(doc));
  ASSERT_EQ(doc.text_objects_on(0).size(), 3u);
  EXPECT_EQ(doc.text_objects_on(0)[0].id, ids[0]);
  EXPECT_EQ(doc.text_objects_on(0)[1].id, ids[1]);
  EXPECT_NE(doc.text_objects_on(0)[2].id, ids[0]);
  EXPECT_EQ(doc.text_objects_on(0)[2].box, add.box_id());
  EXPECT_EQ(doc.text_objects_on(0)[0].line.text, "첫째");
  const auto after_ids = replace.after_ids();
  ASSERT_TRUE(replace.undo(doc));
  ASSERT_EQ(doc.text_objects_on(0).size(), 2u);
  EXPECT_EQ(doc.text_objects_on(0)[0].line.text, "한글");
  EXPECT_EQ(doc.text_objects_on(0)[0].id, ids[0]);
  ASSERT_TRUE(replace.execute(doc));
  ASSERT_EQ(doc.text_objects_on(0).size(), 3u);
  EXPECT_EQ(doc.text_objects_on(0)[2].id, after_ids[2]) << "재실행은 같은 id";
  // 크기만: 원점·내용 유지
  ReplaceTextBoxCommand bigger(0, add.box_id(), test_font(), std::nullopt, 36.0, std::nullopt);
  ASSERT_TRUE(bigger.execute(doc));
  EXPECT_DOUBLE_EQ(doc.text_objects_on(0)[0].line.font_size, 36.0);
  EXPECT_EQ(doc.text_objects_on(0)[0].line.text, "첫째");
  ASSERT_TRUE(bigger.undo(doc));
  EXPECT_DOUBLE_EQ(doc.text_objects_on(0)[0].line.font_size, 24.0);
  // 색만
  ReplaceTextBoxCommand red(0, add.box_id(), test_font(), std::nullopt, std::nullopt, engine::Color{0, 0, 255, 255});
  ASSERT_TRUE(red.execute(doc));
  EXPECT_EQ(doc.text_objects_on(0)[1].line.color, (engine::Color{0, 0, 255, 255}));
  // 아무것도 안 바꾸면 거절
  ReplaceTextBoxCommand nothing(0, add.box_id(), test_font(), std::nullopt, std::nullopt, std::nullopt);
  EXPECT_FALSE(nothing.execute(doc));
  // add 실패 → 보상: 이전 내용 복원
  doc.fail_next_add_text = 1;
  ReplaceTextBoxCommand failing(0, add.box_id(), test_font(), std::string("둘째"), std::nullopt, std::nullopt);
  EXPECT_FALSE(failing.execute(doc));
  ASSERT_EQ(doc.text_objects_on(0).size(), 3u) << "보상으로 되살아난다";
  EXPECT_EQ(doc.text_objects_on(0)[0].line.text, "첫째");
}

// 실제 PDFium: 이동·삭제·수정 실행→취소→재실행, 폰트 캐시, 추출
TEST(TextBoxCommands, ExecuteUndoRedoOnPdfium) {
  auto doc = engine::PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  AddTextCommand add(box(0, "한글 텍스트\n둘째 줄", {72, 300}));
  ASSERT_TRUE(add.execute(*doc));
  const auto ids = add.object_ids();
  const std::string box_id = add.box_id();
  ASSERT_EQ(doc->font_cache_size(), 1);
  engine::RenderOptions render;
  render.scale = 1.0f;
  auto original = doc->render_page(0, render);
  ASSERT_TRUE(original);
  // 이동
  MoveTextBoxCommand move(0, ids, {50, 30});
  ASSERT_TRUE(move.execute(*doc));
  auto list = doc->text_objects(0);
  ASSERT_EQ(list->size(), 2u);
  EXPECT_NEAR((*list)[0].bbox.x, 72 + 50, 2.0);
  ASSERT_TRUE(move.undo(*doc));
  EXPECT_NEAR((*doc->text_objects(0))[0].bbox.x, 72, 2.0);
  auto back = doc->render_page(0, render);
  int diff = 0;
  for (std::size_t i = 0; i < original->pixels.size(); ++i) diff += original->pixels[i] != back->pixels[i];
  EXPECT_EQ(diff, 0) << "이동 취소 뒤 렌더가 같다";
  ASSERT_TRUE(move.execute(*doc));
  ASSERT_TRUE(move.undo(*doc));
  // 삭제 → 취소: 같은 id·상자, 폰트 캐시 재사용, 렌더 동일
  RemoveTextBoxCommand remove(0, box_id, test_font());
  ASSERT_TRUE(remove.execute(*doc));
  EXPECT_TRUE(doc->text_objects(0)->empty());
  ASSERT_TRUE(remove.undo(*doc));
  list = doc->text_objects(0);
  ASSERT_EQ(list->size(), 2u);
  EXPECT_EQ((*list)[0].id, ids[0]);
  EXPECT_EQ((*list)[0].box, box_id);
  EXPECT_EQ((*list)[0].text, "한글 텍스트");
  EXPECT_EQ(doc->font_cache_size(), 1) << "같은 서브셋 key";
  auto restored = doc->render_page(0, render);
  diff = 0;
  for (std::size_t i = 0; i < original->pixels.size(); ++i) diff += original->pixels[i] != restored->pixels[i];
  EXPECT_EQ(diff, 0) << "삭제 취소 뒤 렌더가 같다";
  ASSERT_TRUE(remove.execute(*doc));
  ASSERT_TRUE(remove.undo(*doc));
  // 수정: 내용 + 크기
  ReplaceTextBoxCommand replace(0, box_id, test_font(), std::string("저장 줄"), 36.0, std::nullopt);
  ASSERT_TRUE(replace.execute(*doc));
  list = doc->text_objects(0);
  ASSERT_EQ(list->size(), 1u);
  EXPECT_EQ((*list)[0].id, ids[0]) << "첫 줄 id 재사용";
  EXPECT_EQ((*list)[0].text, "저장 줄");
  EXPECT_FLOAT_EQ(static_cast<float>((*list)[0].font_size), 36.0f);
  EXPECT_NE(doc->load_text(0)->all_text().find("저장 줄"), std::string::npos);
  EXPECT_EQ(doc->load_text(0)->all_text().find("둘째 줄"), std::string::npos);
  ASSERT_TRUE(replace.undo(*doc));
  list = doc->text_objects(0);
  ASSERT_EQ(list->size(), 2u);
  EXPECT_EQ((*list)[1].text, "둘째 줄");
  ASSERT_TRUE(replace.execute(*doc));
  EXPECT_EQ(doc->text_objects(0)->size(), 1u);
}

}  // namespace
}  // namespace onpdf::edit
