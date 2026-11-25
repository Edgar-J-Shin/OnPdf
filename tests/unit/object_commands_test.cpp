// 쪽 객체 Command (#49): 이미지 추가·이동·크기·삭제 실행 → 취소 → 재실행 (가짜 + PDFium), 보관
// 해제, 기존 이미지 마크, 병합
#include "onpdf/edit/object_commands.h"

#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/edit/add_text_command.h"
#include "onpdf/edit/undo_stack.h"

#include "../fake_document.h"
#include "../test_support.h"
#include <algorithm>
#include <gtest/gtest.h>

namespace onpdf::edit {
namespace {

using onpdf::test::FakeDocument;
using onpdf::test::testdata;

engine::ImageSpec pixels_spec(Rect rect) {
  engine::ImagePixels p;
  p.width = 8;
  p.height = 6;
  p.bgra.assign(8 * 6 * 4, 200);
  engine::ImageSpec s;
  s.source = p;
  s.rect = rect;
  return s;
}

TEST(ObjectCommands, AddImageExecuteUndoRedoOnFake) {
  FakeDocument doc({{612, 792}});
  AddImageCommand add(0, pixels_spec({10, 20, 80, 60}));
  EXPECT_EQ(add.id(), "add_image");
  EXPECT_FALSE(add.undo(doc));
  ASSERT_TRUE(add.execute(doc));
  ASSERT_EQ(doc.images_on(0).size(), 1u);
  const std::string id = add.object_id();
  EXPECT_EQ(doc.images_on(0)[0].id, id);
  ASSERT_TRUE(add.undo(doc));
  EXPECT_TRUE(doc.images_on(0).empty());
  EXPECT_EQ(doc.detached_count(), 1) << "떼어 보관";
  ASSERT_TRUE(add.execute(doc));
  ASSERT_EQ(doc.images_on(0).size(), 1u);
  EXPECT_EQ(doc.images_on(0)[0].id, id) << "같은 객체를 다시 붙인다";
  EXPECT_EQ(doc.detached_count(), 0);
  ASSERT_TRUE(add.undo(doc));
  add.discard(doc);
  EXPECT_EQ(doc.detached_count(), 0) << "밀려나면 보관 객체 해제";
  doc.fail_add_image = 1;
  AddImageCommand failing(0, pixels_spec({0, 0, 10, 10}));
  EXPECT_FALSE(failing.execute(doc));
}

TEST(ObjectCommands, MoveResizeRemoveOnFake) {
  FakeDocument doc({{612, 792}});
  AddImageCommand add(0, pixels_spec({10, 20, 80, 60}));
  ASSERT_TRUE(add.execute(doc));
  const std::string id = add.object_id();
  MoveObjectsCommand move(0, {id}, {5, 7});
  EXPECT_EQ(move.id(), "move_object");
  ASSERT_TRUE(move.execute(doc));
  EXPECT_DOUBLE_EQ(doc.images_on(0)[0].rect.x, 15);
  ASSERT_TRUE(move.undo(doc));
  EXPECT_DOUBLE_EQ(doc.images_on(0)[0].rect.x, 10);
  ASSERT_TRUE(move.execute(doc));
  ResizeObjectCommand resize(0, id, doc.images_on(0)[0].rect, {15, 27, 160, 120});
  EXPECT_EQ(resize.id(), "resize_object");
  ASSERT_TRUE(resize.execute(doc));
  EXPECT_DOUBLE_EQ(doc.images_on(0)[0].rect.w, 160);
  ASSERT_TRUE(resize.undo(doc));
  EXPECT_DOUBLE_EQ(doc.images_on(0)[0].rect.w, 80);
  ASSERT_TRUE(resize.execute(doc));
  RemoveObjectsCommand remove(0, {id});
  EXPECT_EQ(remove.id(), "remove_object");
  ASSERT_TRUE(remove.execute(doc));
  EXPECT_TRUE(doc.images_on(0).empty());
  EXPECT_EQ(doc.detached_count(), 1);
  ASSERT_TRUE(remove.undo(doc));
  ASSERT_EQ(doc.images_on(0).size(), 1u);
  EXPECT_DOUBLE_EQ(doc.images_on(0)[0].rect.w, 160) << "같은 객체 (크기 유지)";
  ASSERT_TRUE(remove.execute(doc));
  remove.discard(doc);
  EXPECT_EQ(doc.detached_count(), 0);
  // 텍스트 상자 줄도 같은 이동·삭제
  auto face = FontFace::load(testdata("fonts") / "NotoSansKR-subset.ttf");
  ASSERT_TRUE(face);
  TextBoxSpec spec;
  spec.page = 0;
  spec.origin = {72, 100};
  spec.utf8 = "한글\n둘째";
  spec.face = *face;
  AddTextCommand text(spec);
  ASSERT_TRUE(text.execute(doc));
  RemoveObjectsCommand remove_text(0, text.object_ids());
  ASSERT_TRUE(remove_text.execute(doc));
  EXPECT_TRUE(doc.text_objects_on(0).empty());
  ASSERT_TRUE(remove_text.undo(doc));
  ASSERT_EQ(doc.text_objects_on(0).size(), 2u);
  EXPECT_EQ(doc.text_objects_on(0)[0].id, text.object_ids()[0]);
}

TEST(ObjectCommands, ForeignImageIsMarkedOnFirstEdit) {
  FakeDocument doc({{612, 792}});
  doc.add_foreign_image(0, {50, 50, 100, 100});
  const std::string foreign = doc.images_on(0)[0].id;
  ASSERT_EQ(foreign.rfind("#obj", 0), 0u);
  MoveObjectsCommand move(0, {foreign}, {10, 0});
  ASSERT_TRUE(move.execute(doc));
  ASSERT_EQ(move.ids().size(), 1u);
  EXPECT_NE(move.ids()[0], foreign) << "마크 id 로 바뀐다";
  EXPECT_TRUE(doc.images_on(0)[0].marked);
  ASSERT_TRUE(move.undo(doc));
  EXPECT_DOUBLE_EQ(doc.images_on(0)[0].rect.x, 50);
  ASSERT_TRUE(move.execute(doc));
  EXPECT_DOUBLE_EQ(doc.images_on(0)[0].rect.x, 60);
  doc.add_foreign_image(0, {300, 300, 50, 50});
  const std::string second = doc.images_on(0)[1].id;
  ResizeObjectCommand resize(0, second, {300, 300, 50, 50}, {300, 300, 25, 25});
  ASSERT_TRUE(resize.execute(doc));
  EXPECT_NE(resize.object_id(), second);
  ASSERT_TRUE(resize.undo(doc));
  EXPECT_DOUBLE_EQ(doc.images_on(0)[1].rect.w, 50);
}

TEST(ObjectCommands, KeyboardMovesMergeInUndoStack) {
  FakeDocument doc({{612, 792}});
  AddImageCommand add(0, pixels_spec({10, 20, 80, 60}));
  ASSERT_TRUE(add.execute(doc));
  const std::string id = add.object_id();
  UndoStack stack;
  auto push = [&](std::shared_ptr<Command> c) {
    EXPECT_TRUE(c->execute(doc));
    stack.push(c);
  };
  push(std::make_shared<MoveObjectsCommand>(0, std::vector<std::string>{id}, Point{1, 0}, true));
  push(std::make_shared<MoveObjectsCommand>(0, std::vector<std::string>{id}, Point{1, 0}, true));
  push(std::make_shared<MoveObjectsCommand>(0, std::vector<std::string>{id}, Point{0, 10}, true));
  EXPECT_EQ(stack.size(), 1u) << "키보드 연속 이동은 한 단계";
  EXPECT_DOUBLE_EQ(doc.images_on(0)[0].rect.x, 12);
  ASSERT_TRUE(stack.undo_target()->undo(doc));
  stack.did_undo();
  EXPECT_DOUBLE_EQ(doc.images_on(0)[0].rect.x, 10);
  EXPECT_DOUBLE_EQ(doc.images_on(0)[0].rect.y, 20);
  // 마우스 이동은 합치지 않는다
  push(std::make_shared<MoveObjectsCommand>(0, std::vector<std::string>{id}, Point{5, 0}));
  push(std::make_shared<MoveObjectsCommand>(0, std::vector<std::string>{id}, Point{5, 0}));
  EXPECT_EQ(stack.size(), 2u);
  // 저장 지점 뒤에는 합치지 않는다
  stack.clear();
  push(std::make_shared<MoveObjectsCommand>(0, std::vector<std::string>{id}, Point{1, 0}, true));
  stack.set_clean();
  push(std::make_shared<MoveObjectsCommand>(0, std::vector<std::string>{id}, Point{1, 0}, true));
  EXPECT_EQ(stack.size(), 2u);
}

// 실제 PDFium: 이미지 추가·이동·크기·삭제 실행→취소→재실행, 재실행은 같은 객체, 기존
// 이미지(scanned.pdf)
TEST(ObjectCommands, ExecuteUndoRedoOnPdfium) {
  auto doc = engine::PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  AddImageCommand add(0, pixels_spec({100, 100, 80, 60}));
  ASSERT_TRUE(add.execute(*doc));
  const std::string id = add.object_id();
  auto image_rect = [&]() -> Rect {
    for (const auto& o : *doc->page_objects(0))
      if (o.id == id) return o.bbox;
    return {};
  };
  EXPECT_NEAR(image_rect().x, 100, 0.5);
  ASSERT_TRUE(add.undo(*doc));
  EXPECT_EQ(doc->detached_count(), 1);
  EXPECT_DOUBLE_EQ(image_rect().w, 0);
  ASSERT_TRUE(add.execute(*doc));
  EXPECT_NEAR(image_rect().x, 100, 0.5) << "같은 객체 재부착";
  MoveObjectsCommand move(0, {id}, {20, 30});
  ASSERT_TRUE(move.execute(*doc));
  EXPECT_NEAR(image_rect().x, 120, 0.5);
  ASSERT_TRUE(move.undo(*doc));
  ASSERT_TRUE(move.execute(*doc));
  ResizeObjectCommand resize(0, id, image_rect(), {120, 130, 40, 30});
  ASSERT_TRUE(resize.execute(*doc));
  EXPECT_NEAR(image_rect().w, 40, 0.5);
  ASSERT_TRUE(resize.undo(*doc));
  EXPECT_NEAR(image_rect().w, 80, 0.5);
  ASSERT_TRUE(resize.execute(*doc));
  RemoveObjectsCommand remove(0, {id});
  ASSERT_TRUE(remove.execute(*doc));
  EXPECT_DOUBLE_EQ(image_rect().w, 0);
  ASSERT_TRUE(remove.undo(*doc));
  EXPECT_NEAR(image_rect().w, 40, 0.5);
  ASSERT_TRUE(remove.execute(*doc));
  remove.discard(*doc);
  EXPECT_EQ(doc->detached_count(), 0);

  auto scanned = engine::PdfiumDocument::open(testdata("scanned.pdf"));
  ASSERT_TRUE(scanned);
  const auto foreign = scanned->page_objects(0)->front();
  ASSERT_FALSE(foreign.marked);
  MoveObjectsCommand move_foreign(0, {foreign.id}, {-10, -10});
  ASSERT_TRUE(move_foreign.execute(*scanned));
  const auto moved = scanned->page_objects(0)->front();
  EXPECT_TRUE(moved.marked);
  EXPECT_NEAR(moved.bbox.x, foreign.bbox.x - 10, 0.5);
  ASSERT_TRUE(move_foreign.undo(*scanned));
  EXPECT_NEAR(scanned->page_objects(0)->front().bbox.x, foreign.bbox.x, 0.5);
}

// #66: 떼어 둔 객체의 쪽 번호는 쪽 삭제·이동·삽입 뒤에도 맞아야 한다 (삭제된 쪽의 것은 lost)
TEST(DetachedObjects, PageIndexFollowsStructuralEdits) {
  auto doc = engine::PdfiumDocument::open(testdata("simple.pdf"));  // 2쪽
  ASSERT_TRUE(doc);
  engine::ImagePixels px{4, 4, std::vector<std::uint8_t>(64, 0)};
  for (std::size_t k = 0; k < px.bgra.size(); k += 4) px.bgra[k + 3] = 255;
  auto image = doc->add_image_object(1, engine::ImageSpec{px, Rect{100, 100, 40, 40}, {}});
  ASSERT_TRUE(image);
  auto count_on = [&](int page) {
    auto objects = doc->page_objects(page);
    EXPECT_TRUE(objects);
    return objects
               ? std::count_if(objects->begin(), objects->end(),
                               [](const auto& o) { return o.kind == engine::ObjectKind::kImage; })
               : -1;
  };
  // 삭제: 0쪽을 지우면 1쪽 → 0쪽
  auto d1 = doc->detach_objects(1, {*image});
  ASSERT_TRUE(d1);
  ASSERT_TRUE(doc->delete_pages({0}));
  ASSERT_TRUE(doc->reattach_objects(*d1));
  EXPECT_EQ(count_on(0), 1);
  // 삽입: 앞에 한 쪽을 넣으면 0쪽 → 1쪽
  auto d2 = doc->detach_objects(0, {*image});
  ASSERT_TRUE(d2);
  auto snapshot = doc->copy_pages_to_snapshot({0});  // 뗀 뒤에 복사해야 사본에 이미지가 없다
  ASSERT_TRUE(snapshot);
  ASSERT_TRUE(doc->insert_from_snapshot(*snapshot, {0}, 0));
  ASSERT_TRUE(doc->reattach_objects(*d2));
  EXPECT_EQ(count_on(1), 1);
  EXPECT_EQ(count_on(0), 0);
  // 이동: 1쪽을 0 으로 옮기면 1쪽 → 0쪽
  auto d3 = doc->detach_objects(1, {*image});
  ASSERT_TRUE(d3);
  ASSERT_TRUE(doc->move_pages({1}, 0));
  ASSERT_TRUE(doc->reattach_objects(*d3));
  EXPECT_EQ(count_on(0), 1);
  // 그 쪽을 지우면 되돌릴 수 없다
  auto d4 = doc->detach_objects(0, {*image});
  ASSERT_TRUE(d4);
  ASSERT_TRUE(doc->delete_pages({0}));
  auto lost = doc->reattach_objects(*d4);
  ASSERT_FALSE(lost);
  EXPECT_EQ(lost.error().code, ErrorCode::kInvalidState);
  doc->drop_detached(*d4);
  EXPECT_EQ(doc->detached_count(), 0);
}
}  // namespace
}  // namespace onpdf::edit
