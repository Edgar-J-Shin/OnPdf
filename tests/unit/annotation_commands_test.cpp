// 주석 Command 4종: 실행 → 취소 → 재실행 (가짜 + PDFium), 다중 쪽 보상, "#index" id (#35)
#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/edit/annotation_commands.h"

#include "../fake_document.h"
#include "../test_support.h"

#include <gtest/gtest.h>

namespace onpdf::edit {
namespace {

using engine::AnnotationKind;
using engine::AnnotationSpec;
using onpdf::test::FakeDocument;
using onpdf::test::testdata;

AnnotationSpec highlight(onpdf::Rect quad) {
  AnnotationSpec s;
  s.kind = AnnotationKind::kHighlight;
  s.quads = {quad};
  return s;
}

TEST(AddAnnotationsCommand, ExecuteUndoRedoOnFake) {
  FakeDocument doc({{612, 792}, {612, 792}});
  AddAnnotationsCommand cmd({{0, highlight({10, 10, 100, 12})}, {1, highlight({10, 30, 100, 12})}});
  EXPECT_EQ(cmd.id(), "add_highlight");
  auto change = cmd.execute(doc);
  ASSERT_TRUE(change) << change.error().message;
  EXPECT_EQ(change->pages, (std::vector<int>{0, 1}));
  EXPECT_FALSE(change->structural);
  EXPECT_EQ(doc.annotation_count(0), 1);
  EXPECT_EQ(doc.annotation_count(1), 1);
  const std::string id0 = cmd.items()[0].second.id;
  EXPECT_FALSE(id0.empty());
  ASSERT_TRUE(cmd.undo(doc));
  EXPECT_EQ(doc.annotation_count(0), 0);
  EXPECT_EQ(doc.annotation_count(1), 0);
  ASSERT_TRUE(cmd.execute(doc));  // 재실행: 같은 NM
  EXPECT_EQ(cmd.items()[0].second.id, id0);
  EXPECT_NE(doc.annotation(0, id0), nullptr);
}

TEST(AddAnnotationsCommand, FailureOnSecondPageCompensatesFirst) {
  FakeDocument doc({{612, 792}, {612, 792}});
  doc.fail_add_on_page = 1;
  AddAnnotationsCommand cmd({{0, highlight({10, 10, 100, 12})}, {1, highlight({10, 30, 100, 12})}});
  EXPECT_FALSE(cmd.execute(doc));
  EXPECT_EQ(doc.annotation_count(0), 0) << "first page annotation must be rolled back";
  EXPECT_FALSE(cmd.undo(doc));  // 실행 전 undo
  AddAnnotationsCommand empty({});
  EXPECT_FALSE(empty.execute(doc));
}

TEST(RemoveAnnotationCommand, ExecuteUndoRedoOnFake) {
  FakeDocument doc({{612, 792}});
  AnnotationSpec note;
  note.kind = AnnotationKind::kText;
  note.rect = {50, 50, 20, 20};
  note.contents = "메모";
  ASSERT_TRUE(doc.add_annotation(0, note));
  RemoveAnnotationCommand cmd(0, "#0");  // NM 없는 것처럼 index 로 지정
  auto change = cmd.execute(doc);
  ASSERT_TRUE(change) << change.error().message;
  EXPECT_EQ(doc.annotation_count(0), 0);
  EXPECT_NE(cmd.annotation_id(), "#0");  // 스냅샷이 NM 을 부여했다
  ASSERT_TRUE(cmd.undo(doc));
  ASSERT_EQ(doc.annotation_count(0), 1);
  EXPECT_EQ(doc.annotation(0, cmd.annotation_id())->snap.contents, "메모");
  ASSERT_TRUE(cmd.execute(doc));
  EXPECT_EQ(doc.annotation_count(0), 0);
  RemoveAnnotationCommand missing(0, "nope");
  EXPECT_FALSE(missing.execute(doc));
  EXPECT_FALSE(missing.undo(doc));
}

TEST(SetAnnotationColorCommand, UndoRestoresAppearance) {
  FakeDocument doc({{612, 792}});
  auto id = doc.add_annotation(0, highlight({10, 10, 100, 12}));
  ASSERT_TRUE(id);
  doc.fake_render(0);  // AP 생성
  ASSERT_TRUE(doc.annotation(0, *id)->has_ap);
  SetAnnotationColorCommand cmd(0, *id, {0, 255, 0, 255});
  ASSERT_TRUE(cmd.execute(doc));
  EXPECT_FALSE(doc.annotation(0, *id)->has_ap) << "AP 를 지우고 색을 쓴다";
  EXPECT_EQ(doc.annotation(0, *id)->snap.color, (engine::Color{0, 255, 0, 255}));
  ASSERT_TRUE(cmd.undo(doc));
  EXPECT_EQ(doc.annotation(0, *id)->snap.color, (engine::Color{255, 255, 0, 255})) << "이전 색으로 (모양은 재생성)";
  ASSERT_TRUE(cmd.execute(doc));
  EXPECT_EQ(doc.annotation(0, *id)->snap.color, (engine::Color{0, 255, 0, 255}));
}

TEST(SetAnnotationContentsCommand, ExecuteUndoRedoOnFake) {
  FakeDocument doc({{612, 792}});
  AnnotationSpec note;
  note.kind = AnnotationKind::kText;
  note.rect = {50, 50, 20, 20};
  note.contents = "처음";
  auto id = doc.add_annotation(0, note);
  ASSERT_TRUE(id);
  SetAnnotationContentsCommand cmd(0, *id, "바뀜");
  ASSERT_TRUE(cmd.execute(doc));
  EXPECT_EQ(doc.annotation(0, *id)->snap.contents, "바뀜");
  ASSERT_TRUE(cmd.undo(doc));
  EXPECT_EQ(doc.annotation(0, *id)->snap.contents, "처음");
  ASSERT_TRUE(cmd.execute(doc));
  EXPECT_EQ(doc.annotation(0, *id)->snap.contents, "바뀜");
}

// CLAUDE.md: 실제 PDFium 에서도 실행 → 취소 → 재실행
TEST(AnnotationCommands, ExecuteUndoRedoOnPdfium) {
  auto doc = engine::PdfiumDocument::open(testdata("annotations.pdf"));
  ASSERT_TRUE(doc);
  ASSERT_EQ(doc->annotations(0)->size(), 6u);
  // 추가
  AddAnnotationsCommand add({{0, highlight({72, 100, 200, 14})}, {1, highlight({72, 100, 200, 14})}});
  ASSERT_TRUE(add.execute(*doc));
  EXPECT_EQ(doc->annotations(0)->size(), 7u);
  EXPECT_EQ(doc->annotations(1)->size(), 1u);
  ASSERT_TRUE(add.undo(*doc));
  EXPECT_EQ(doc->annotations(0)->size(), 6u);
  EXPECT_EQ(doc->annotations(1)->size(), 0u);
  ASSERT_TRUE(add.execute(*doc));
  EXPECT_EQ(doc->annotations(0)->size(), 7u);
  // 기존 Square 삭제 (index id) → 취소 → 재실행
  RemoveAnnotationCommand remove(0, "#1");
  ASSERT_TRUE(remove.execute(*doc));
  EXPECT_EQ(doc->annotations(0)->size(), 6u);
  ASSERT_TRUE(remove.undo(*doc));
  auto list = doc->annotations(0);
  ASSERT_EQ(list->size(), 7u);
  bool square_back = false;
  for (const auto& a : *list) square_back = square_back || (a.kind == AnnotationKind::kSquare && a.id == remove.annotation_id());
  EXPECT_TRUE(square_back);
  ASSERT_TRUE(remove.execute(*doc));
  EXPECT_EQ(doc->annotations(0)->size(), 6u);
  // 기존 하이라이트(AP 있음) 색 변경 → 취소하면 AP 그대로
  auto before = doc->snapshot_annotation(0, "#0");
  ASSERT_TRUE(before);
  SetAnnotationColorCommand color(0, before->id, {0, 255, 0, 255});
  ASSERT_TRUE(color.execute(*doc));
  auto mid = doc->snapshot_annotation(0, before->id);
  EXPECT_EQ(mid->color, (engine::Color{0, 255, 0, 255}));
  ASSERT_TRUE(color.undo(*doc));
  EXPECT_EQ(doc->snapshot_annotation(0, before->id)->color, before->color);
  ASSERT_TRUE(color.execute(*doc));
  // 메모 내용
  SetAnnotationContentsCommand contents(0, "#2", "새 내용");
  ASSERT_TRUE(contents.execute(*doc));
  ASSERT_TRUE(contents.undo(*doc));
  ASSERT_TRUE(contents.execute(*doc));
  bool found = false;
  for (const auto& a : *doc->annotations(0)) found = found || a.contents == "새 내용";
  EXPECT_TRUE(found);
}

}  // namespace
}  // namespace onpdf::edit
