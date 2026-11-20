// 폼 Command (#51): 텍스트·체크·라디오·선택지 실행 → 취소 → 재실행 (가짜 + PDFium), 라디오 첫 선택 취소 불가
#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/edit/form_commands.h"

#include "../fake_document.h"
#include "../test_support.h"

#include <gtest/gtest.h>

namespace onpdf::edit {
namespace {

using onpdf::test::FakeDocument;
using onpdf::test::testdata;

engine::FormWidgetInfo text_widget(const std::string& name, int control, const std::string& value) {
  engine::FormWidgetInfo w;
  w.ref = {name, control};
  w.type = engine::FieldType::kText;
  w.value = value;
  return w;
}

engine::FormWidgetInfo radio_widget(const std::string& name, int control, const std::string& on, bool checked) {
  engine::FormWidgetInfo w;
  w.ref = {name, control};
  w.type = engine::FieldType::kRadio;
  w.export_value = on;
  w.checked = checked;
  w.value = checked ? on : "Off";
  return w;
}

engine::FormWidgetInfo widget(engine::EditableDocument& doc, int page, const std::string& name, int control = 0) {
  auto list = doc.form_widgets(page);
  EXPECT_TRUE(list);
  for (const auto& w : *list) if (w.ref.name == name && w.ref.control_index == control) return w;
  ADD_FAILURE() << "no widget " << name;
  return {};
}

TEST(FormCommands, TextExecuteUndoRedoOnFakeAcrossPages) {
  FakeDocument doc({{612, 792}, {612, 792}});
  doc.add_widget(0, text_widget("name", 0, "Hong"));
  doc.add_widget(1, text_widget("name", 1, "Hong"));
  SetFormTextCommand cmd(0, {"name", 0}, "홍길동");
  EXPECT_EQ(cmd.id(), "form_text");
  EXPECT_FALSE(cmd.undo(doc));
  auto change = cmd.execute(doc);
  ASSERT_TRUE(change);
  EXPECT_EQ(change->pages, (std::vector<int>{0, 1}));
  EXPECT_EQ(cmd.before(), "Hong");
  EXPECT_EQ(doc.widget(1, {"name", 1})->value, "홍길동");
  ASSERT_TRUE(cmd.undo(doc));
  EXPECT_EQ(doc.widget(0, {"name", 0})->value, "Hong");
  EXPECT_EQ(doc.widget(1, {"name", 1})->value, "Hong");
  ASSERT_TRUE(cmd.execute(doc));
  EXPECT_EQ(doc.widget(0, {"name", 0})->value, "홍길동");
  SetFormTextCommand same(0, {"name", 0}, "홍길동");
  EXPECT_FALSE(same.execute(doc)) << "값이 같으면 거절";
  SetFormTextCommand missing(0, {"nope", 0}, "x");
  EXPECT_FALSE(missing.execute(doc));
  doc.fail_form = true;
  EXPECT_FALSE(cmd.undo(doc));
}

TEST(FormCommands, CheckBoxAndRadioOnFake) {
  FakeDocument doc({{612, 792}});
  engine::FormWidgetInfo box;
  box.ref = {"agree", 0};
  box.type = engine::FieldType::kCheckBox;
  box.export_value = "Yes";
  box.value = "Off";
  doc.add_widget(0, box);
  SetFormCheckedCommand check(0, {"agree", 0}, true);
  EXPECT_EQ(check.id(), "form_check");
  ASSERT_TRUE(check.execute(doc));
  EXPECT_TRUE(doc.widget(0, {"agree", 0})->checked);
  ASSERT_TRUE(check.undo(doc));
  EXPECT_FALSE(doc.widget(0, {"agree", 0})->checked);
  ASSERT_TRUE(check.execute(doc));
  EXPECT_TRUE(check.undoable());
  // 라디오: 이전 선택이 있으면 그것으로 되돌린다
  doc.add_widget(0, radio_widget("size", 0, "S", true));
  doc.add_widget(0, radio_widget("size", 1, "L", false));
  SetFormCheckedCommand pick(0, {"size", 1}, true);
  ASSERT_TRUE(pick.execute(doc));
  EXPECT_TRUE(doc.widget(0, {"size", 1})->checked);
  EXPECT_FALSE(doc.widget(0, {"size", 0})->checked);
  EXPECT_TRUE(pick.undoable());
  ASSERT_TRUE(pick.undo(doc));
  EXPECT_TRUE(doc.widget(0, {"size", 0})->checked);
  EXPECT_FALSE(doc.widget(0, {"size", 1})->checked);
  ASSERT_TRUE(pick.execute(doc));
  EXPECT_TRUE(doc.widget(0, {"size", 1})->checked);
  SetFormCheckedCommand clear(0, {"size", 1}, false);
  auto cleared = clear.execute(doc);
  ASSERT_FALSE(cleared);
  EXPECT_EQ(cleared.error().code, ErrorCode::kUnsupported);
}

TEST(FormCommands, FirstRadioChoiceCannotBeUndone) {
  FakeDocument doc({{612, 792}});
  doc.add_widget(0, radio_widget("gender", 0, "M", false));
  doc.add_widget(0, radio_widget("gender", 1, "F", false));
  SetFormCheckedCommand pick(0, {"gender", 0}, true);
  ASSERT_TRUE(pick.execute(doc));
  EXPECT_FALSE(pick.undoable());
  auto undone = pick.undo(doc);
  ASSERT_FALSE(undone);
  EXPECT_EQ(undone.error().code, ErrorCode::kUnsupported);
  EXPECT_TRUE(doc.widget(0, {"gender", 0})->checked) << "문서는 그대로";
}

TEST(FormCommands, ChoiceExecuteUndoRedoOnFake) {
  FakeDocument doc({{612, 792}});
  engine::FormWidgetInfo list;
  list.ref = {"subjects", 0};
  list.type = engine::FieldType::kListBox;
  list.multi_select = true;
  list.options = {"Math", "Science", "Art"};
  list.selected = {1};
  list.value = "Science";
  doc.add_widget(0, list);
  SetFormChoiceCommand cmd(0, {"subjects", 0}, {2, 0});
  EXPECT_EQ(cmd.id(), "form_choice");
  ASSERT_TRUE(cmd.execute(doc));
  EXPECT_EQ(doc.widget(0, {"subjects", 0})->selected, (std::vector<int>{0, 2}));
  ASSERT_TRUE(cmd.undo(doc));
  EXPECT_EQ(doc.widget(0, {"subjects", 0})->selected, (std::vector<int>{1}));
  ASSERT_TRUE(cmd.execute(doc));
  EXPECT_EQ(doc.widget(0, {"subjects", 0})->selected, (std::vector<int>{0, 2}));
  SetFormChoiceCommand same(0, {"subjects", 0}, {0, 2});
  EXPECT_FALSE(same.execute(doc));
}

// 실제 PDFium: 세 Command 의 실행 → 취소 → 재실행 상태 비교
TEST(FormCommands, RoundTripOnPdfium) {
  auto doc = engine::PdfiumDocument::open(testdata("form-fields.pdf"));
  ASSERT_TRUE(doc);
  SetFormTextCommand text(0, {"name", 0}, "홍길동");
  auto change = text.execute(*doc);
  ASSERT_TRUE(change) << change.error().message;
  EXPECT_EQ(change->pages, (std::vector<int>{0, 1}));
  ASSERT_TRUE(text.undo(*doc));
  EXPECT_EQ(widget(*doc, 1, "name", 1).value, "Hong");
  ASSERT_TRUE(text.execute(*doc));
  EXPECT_EQ(widget(*doc, 1, "name", 1).value, "홍길동");

  SetFormTextCommand address(0, {"address", 0}, "1 Main St\n2nd line");
  ASSERT_TRUE(address.execute(*doc));
  EXPECT_EQ(widget(*doc, 0, "address").value, "1 Main St\n2nd line") << "줄바꿈 정규화로 Command 값과 같다";
  ASSERT_TRUE(address.undo(*doc));
  EXPECT_EQ(widget(*doc, 0, "address").value, "");

  SetFormCheckedCommand agree(0, {"agree", 0}, true);
  ASSERT_TRUE(agree.execute(*doc));
  ASSERT_TRUE(agree.undo(*doc));
  EXPECT_FALSE(widget(*doc, 0, "agree").checked);
  ASSERT_TRUE(agree.execute(*doc));
  EXPECT_TRUE(widget(*doc, 0, "agree").checked);

  SetFormCheckedCommand first(0, {"gender", 0}, true);
  ASSERT_TRUE(first.execute(*doc));
  EXPECT_FALSE(first.undoable());
  SetFormCheckedCommand second(0, {"gender", 2}, true);
  ASSERT_TRUE(second.execute(*doc));
  EXPECT_EQ(widget(*doc, 0, "gender", 2).value, "X");
  ASSERT_TRUE(second.undo(*doc));
  EXPECT_TRUE(widget(*doc, 0, "gender", 0).checked);
  EXPECT_FALSE(widget(*doc, 0, "gender", 2).checked);
  ASSERT_TRUE(second.execute(*doc));
  EXPECT_TRUE(widget(*doc, 0, "gender", 2).checked);
  EXPECT_FALSE(first.undo(*doc)) << "첫 선택은 취소 불가";

  SetFormChoiceCommand city(0, {"city", 0}, {1});
  ASSERT_TRUE(city.execute(*doc));
  EXPECT_EQ(widget(*doc, 0, "city").value, "Busan");
  ASSERT_TRUE(city.undo(*doc));
  EXPECT_EQ(widget(*doc, 0, "city").value, "Seoul");
  ASSERT_TRUE(city.execute(*doc));
  // 직접 입력한 값에서 선택지로 바꾼 뒤 취소하면 직접 입력 값으로
  SetFormTextCommand typed(0, {"city", 0}, "Incheon");
  ASSERT_TRUE(typed.execute(*doc));
  SetFormChoiceCommand back(0, {"city", 0}, {2});
  ASSERT_TRUE(back.execute(*doc));
  EXPECT_EQ(widget(*doc, 0, "city").value, "Daegu");
  ASSERT_TRUE(back.undo(*doc));
  EXPECT_EQ(widget(*doc, 0, "city").value, "Incheon");

  SetFormChoiceCommand subjects(0, {"subjects", 0}, {1, 3});
  ASSERT_TRUE(subjects.execute(*doc));
  ASSERT_TRUE(subjects.undo(*doc));
  EXPECT_TRUE(widget(*doc, 0, "subjects").selected.empty());
  ASSERT_TRUE(subjects.execute(*doc));
  EXPECT_EQ(widget(*doc, 0, "subjects").selected, (std::vector<int>{1, 3}));
}

}  // namespace
}  // namespace onpdf::edit
