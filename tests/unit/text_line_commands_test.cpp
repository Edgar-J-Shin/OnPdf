// ReplaceTextLineCommand·RemoveTextLineCommand (#65): 실행 → 취소 → 재실행 (가짜 + PDFium), discard 가 토큰 해제,
// 대체 글꼴 분기, 앞선 편집(이미지 삭제·주석) 뒤 줄 수정 → 전부 취소
#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/edit/add_text_command.h"
#include "onpdf/edit/annotation_commands.h"
#include "onpdf/edit/object_commands.h"
#include "onpdf/edit/text_line_commands.h"

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

std::string line_id_containing(engine::EditableDocument& doc, int page, const std::string& needle) {
  auto lines = doc.text_lines(page);
  EXPECT_TRUE(lines);
  if (!lines) return {};
  for (const auto& l : *lines) {
    if (l.text.find(needle) != std::string::npos) return l.id;
  }
  return {};
}

std::string page_text(const engine::PdfiumDocument& doc) {
  auto t = doc.load_text(0);
  EXPECT_TRUE(t);
  return t ? t->all_text() : std::string();
}

TEST(TextLineCommands, ReplaceExecuteUndoRedoOnFake) {
  FakeDocument doc({{612, 792}});
  doc.add_line(0, "p/0/abc", "old line");
  ReplaceTextLineCommand cmd(0, "p/0/abc", std::string("new line"), std::nullopt, std::nullopt, test_font());
  EXPECT_EQ(cmd.id(), "replace_line");
  EXPECT_FALSE(cmd.undo(doc)) << "실행 전 취소는 실패";
  auto change = cmd.execute(doc);
  ASSERT_TRUE(change) << change.error().message;
  EXPECT_EQ(change->pages, std::vector<int>{0});
  EXPECT_TRUE(doc.lines_on(0).empty());
  ASSERT_EQ(doc.text_objects_on(0).size(), 1u);
  EXPECT_EQ(doc.text_objects_on(0)[0].line.text, "new line");
  EXPECT_EQ(doc.text_objects_on(0)[0].font_key, "original");
  EXPECT_FALSE(cmd.used_fallback());
  EXPECT_EQ(doc.document_state_count(), 1);
  ASSERT_TRUE(cmd.undo(doc));
  ASSERT_EQ(doc.lines_on(0).size(), 1u);
  EXPECT_TRUE(doc.text_objects_on(0).empty());
  EXPECT_EQ(doc.document_state_count(), 1) << "토큰은 재실행을 위해 남는다";
  ASSERT_TRUE(cmd.execute(doc));
  EXPECT_TRUE(doc.lines_on(0).empty());
  EXPECT_EQ(doc.document_state_count(), 1) << "재실행은 옛 토큰을 버리고 새로 만든다";
  cmd.discard(doc);
  EXPECT_EQ(doc.document_state_count(), 0);
}

TEST(TextLineCommands, FallbackFontWhenOriginalCannotDraw) {
  FakeDocument doc({{612, 792}});
  doc.add_line(0, "p/1/abc", "old", /*covers=*/false);
  ReplaceTextLineCommand without(0, "p/1/abc", std::string("한글"), std::nullopt, std::nullopt, nullptr);
  auto refused = without.execute(doc);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code, ErrorCode::kUnsupported);
  ReplaceTextLineCommand cmd(0, "p/1/abc", std::string("한글"), std::nullopt, std::nullopt, test_font());
  ASSERT_TRUE(cmd.execute(doc));
  EXPECT_TRUE(cmd.used_fallback());
  EXPECT_EQ(doc.font_count(), 1);
  ASSERT_TRUE(cmd.undo(doc));
  ASSERT_EQ(doc.lines_on(0).size(), 1u);
}

TEST(TextLineCommands, RemoveExecuteUndoRedoOnFakeAndFailureLeavesNoToken) {
  FakeDocument doc({{612, 792}});
  doc.add_line(0, "p/2/abc", "gone");
  doc.add_line(0, "p/3/def", "locked", true, /*editable=*/false);
  RemoveTextLineCommand cmd(0, "p/2/abc");
  EXPECT_EQ(cmd.id(), "remove_line");
  ASSERT_TRUE(cmd.execute(doc));
  EXPECT_EQ(doc.lines_on(0).size(), 1u);
  ASSERT_TRUE(cmd.undo(doc));
  EXPECT_EQ(doc.lines_on(0).size(), 2u);
  ASSERT_TRUE(cmd.execute(doc));
  cmd.discard(doc);
  EXPECT_EQ(doc.document_state_count(), 0);
  RemoveTextLineCommand locked(0, "p/3/def");
  auto refused = locked.execute(doc);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code, ErrorCode::kUnsupported);
  EXPECT_EQ(doc.document_state_count(), 0);
  doc.fail_next_line_edit = true;
  RemoveTextLineCommand failing(0, "p/2/abc");
  EXPECT_FALSE(failing.execute(doc));
  EXPECT_FALSE(failing.undo(doc));
}

TEST(TextLineCommands, PdfiumReplaceUndoRedoRestoresRender) {
  auto doc = engine::PdfiumDocument::open(testdata("text-lines-type3.pdf"));
  ASSERT_TRUE(doc);
  engine::RenderOptions render;
  render.scale = 1.0f;
  auto before = doc->render_page(0, render);
  ASSERT_TRUE(before);
  const std::string id = line_id_containing(*doc, 0, "한글 텍스트 줄");
  ASSERT_FALSE(id.empty());
  ReplaceTextLineCommand cmd(0, id, std::string("줄 텍스트 한글"), std::nullopt, std::nullopt, test_font());
  ASSERT_TRUE(cmd.execute(*doc));
  EXPECT_FALSE(cmd.used_fallback()) << "임베딩 CID 글꼴 재사용";
  EXPECT_NE(page_text(*doc).find("줄 텍스트 한글"), std::string::npos);
  auto edited = doc->render_page(0, render);
  ASSERT_TRUE(edited);
  EXPECT_NE(edited->pixels, before->pixels);
  ASSERT_TRUE(cmd.undo(*doc));
  auto undone = doc->render_page(0, render);
  ASSERT_TRUE(undone);
  EXPECT_EQ(undone->pixels, before->pixels) << "취소는 바이트 복원이라 픽셀까지 같다";
  EXPECT_NE(page_text(*doc).find("한글 텍스트 줄"), std::string::npos);
  ASSERT_TRUE(cmd.execute(*doc));
  auto redone = doc->render_page(0, render);
  ASSERT_TRUE(redone);
  EXPECT_EQ(redone->pixels, edited->pixels);
  // OnPdf 상자 재수정(수술 없음)과 그 취소
  ReplaceTextLineCommand again(0, cmd.box(), std::string("한글 줄"), std::nullopt, std::nullopt, test_font());
  ASSERT_TRUE(again.execute(*doc));
  EXPECT_NE(page_text(*doc).find("한글 줄"), std::string::npos);
  ASSERT_TRUE(again.undo(*doc));
  EXPECT_NE(page_text(*doc).find("줄 텍스트 한글"), std::string::npos);
  EXPECT_EQ(doc->document_state_count(), 1);
  cmd.discard(*doc);
  EXPECT_EQ(doc->document_state_count(), 0);
}

// 앞선 편집(이미지 추가·삭제, 주석) 뒤 줄 수정 → 전부 취소하면 처음 상태
TEST(TextLineCommands, EarlierEditsStayUndoableAcrossReopen) {
  auto doc = engine::PdfiumDocument::open(testdata("text-lines-basic.pdf"));
  ASSERT_TRUE(doc);
  engine::RenderOptions render;
  render.scale = 1.0f;
  auto initial = doc->render_page(0, render);
  ASSERT_TRUE(initial);
  engine::ImagePixels px{4, 4, std::vector<std::uint8_t>(64, 0)};
  for (std::size_t k = 0; k < px.bgra.size(); k += 4) px.bgra[k + 3] = 255;
  AddImageCommand add_image(0, engine::ImageSpec{px, Rect{300, 100, 40, 40}, {}});
  ASSERT_TRUE(add_image.execute(*doc));
  RemoveObjectsCommand remove_image(0, {add_image.object_id()});
  ASSERT_TRUE(remove_image.execute(*doc));
  EXPECT_EQ(doc->detached_count(), 1);
  engine::AnnotationSpec note;
  note.kind = engine::AnnotationKind::kSquare;
  note.rect = Rect{400, 600, 50, 30};
  note.color = {255, 0, 0, 255};
  AddAnnotationsCommand add_note({{0, note}});
  ASSERT_TRUE(add_note.execute(*doc));
  const std::string id = line_id_containing(*doc, 0, "Delete me entirely");
  ASSERT_FALSE(id.empty());
  RemoveTextLineCommand remove_line(0, id);
  ASSERT_TRUE(remove_line.execute(*doc));
  EXPECT_EQ(doc->detached_count(), 1) << "다시 열어도 떼어 둔 이미지가 남는다";
  ASSERT_EQ(doc->annotations(0)->size(), 1u);
  // 전부 취소
  ASSERT_TRUE(remove_line.undo(*doc));
  ASSERT_TRUE(add_note.undo(*doc));
  ASSERT_TRUE(remove_image.undo(*doc));
  ASSERT_TRUE(add_image.undo(*doc));
  EXPECT_EQ(doc->detached_count(), 1) << "AddImage 취소가 이미지를 떼어 둔다";
  auto final_render = doc->render_page(0, render);
  ASSERT_TRUE(final_render);
  EXPECT_EQ(final_render->pixels, initial->pixels);
  EXPECT_TRUE(doc->annotations(0)->empty());
  remove_line.discard(*doc);
  add_image.discard(*doc);
  EXPECT_EQ(doc->detached_count(), 0);
  EXPECT_EQ(doc->document_state_count(), 0);
}


// #69: 서식만 바꾸면 id 는 format_line, 가짜·PDFium 모두 취소가 크기·색을 되돌린다 (ofont 상자 경로 포함)
TEST(TextLineCommands, FormatOnlyChangesSizeAndColorAndUndoes) {
  FakeDocument doc({{612, 792}});
  doc.add_line(0, "p/0/abc", "keep me");
  ReplaceTextLineCommand nothing(0, "p/0/abc", std::nullopt, std::nullopt, std::nullopt, test_font());
  EXPECT_FALSE(nothing.execute(doc));
  ReplaceTextLineCommand format(0, "p/0/abc", std::nullopt, 24.0, engine::Color{200, 0, 0, 255}, test_font());
  EXPECT_EQ(format.id(), "format_line");
  ASSERT_TRUE(format.execute(doc));
  ASSERT_EQ(doc.text_objects_on(0).size(), 1u);
  EXPECT_EQ(doc.text_objects_on(0)[0].line.text, "keep me") << "글은 그대로";
  EXPECT_DOUBLE_EQ(doc.text_objects_on(0)[0].line.font_size, 24.0);
  EXPECT_EQ(doc.text_objects_on(0)[0].line.color.r, 200);
  ASSERT_TRUE(format.undo(doc));
  EXPECT_EQ(doc.lines_on(0).size(), 1u);
  ASSERT_TRUE(format.execute(doc));
  format.discard(doc);
  EXPECT_EQ(doc.document_state_count(), 0);

  auto pdf = engine::PdfiumDocument::open(testdata("text-lines-type3.pdf"));
  ASSERT_TRUE(pdf);
  const std::string id = line_id_containing(*pdf, 0, "한글 텍스트 줄");
  ASSERT_FALSE(id.empty());
  ReplaceTextLineCommand bigger(0, id, std::nullopt, 24.0, std::nullopt, test_font());
  ASSERT_TRUE(bigger.execute(*pdf));
  EXPECT_FALSE(bigger.used_fallback()) << "원본 글꼴 유지";
  auto objects = pdf->text_objects(0);
  ASSERT_TRUE(objects);
  ASSERT_EQ(objects->size(), 1u);
  EXPECT_NEAR((*objects)[0].font_size, 24.0, 0.05);
  EXPECT_TRUE((*objects)[0].original_font);
  // ofont 상자의 색 변경(수술 없음) → 취소가 크기·색 복원
  ReplaceTextLineCommand red(0, bigger.box(), std::nullopt, std::nullopt, engine::Color{200, 0, 0, 255}, test_font());
  EXPECT_EQ(red.id(), "format_line");
  ASSERT_TRUE(red.execute(*pdf));
  objects = pdf->text_objects(0);
  ASSERT_EQ(objects->size(), 1u);
  EXPECT_EQ((*objects)[0].color.r, 200);
  EXPECT_NEAR((*objects)[0].font_size, 24.0, 0.05);
  ASSERT_TRUE(red.undo(*pdf));
  objects = pdf->text_objects(0);
  ASSERT_EQ(objects->size(), 1u);
  EXPECT_EQ((*objects)[0].color.r, 0);
  EXPECT_NEAR((*objects)[0].font_size, 24.0, 0.05);
  EXPECT_EQ((*objects)[0].text, "한글 텍스트 줄");
  ASSERT_TRUE(bigger.undo(*pdf));
  EXPECT_TRUE(pdf->text_objects(0)->empty());
  bigger.discard(*pdf);
}
}  // namespace
}  // namespace onpdf::edit
