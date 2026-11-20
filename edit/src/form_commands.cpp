#include "onpdf/edit/form_commands.h"

#include <algorithm>
#include <utility>

namespace onpdf::edit {
namespace {

Result<engine::FormWidgetInfo> read_widget(engine::EditableDocument& doc, int page, const engine::FieldRef& ref) {
  auto list = doc.form_widgets(page);
  if (!list) return std::unexpected(list.error());
  for (const auto& w : *list) if (w.ref == ref) return w;
  return make_error(ErrorCode::kInvalidArgument, "form field not found: " + ref.name);
}

engine::ChangeSet pages_change(std::vector<int> pages) {
  engine::ChangeSet change;
  std::sort(pages.begin(), pages.end());
  pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
  change.pages = std::move(pages);
  return change;
}

}  // namespace

// ---------------------------------------------------------------- 텍스트

SetFormTextCommand::SetFormTextCommand(int page, engine::FieldRef ref, std::string utf8)
    : page_(page), ref_(std::move(ref)), after_(std::move(utf8)) {}

Result<engine::ChangeSet> SetFormTextCommand::execute(engine::EditableDocument& doc) {
  if (!executed_) {
    auto widget = read_widget(doc, page_, ref_);
    if (!widget) return std::unexpected(widget.error());
    if (widget->value == after_) return make_error(ErrorCode::kInvalidArgument, "value unchanged");
    before_ = widget->value;
  }
  auto pages = doc.set_form_text(page_, ref_, after_);
  if (!pages) return std::unexpected(pages.error());
  executed_ = true;
  return pages_change(std::move(*pages));
}

Result<engine::ChangeSet> SetFormTextCommand::undo(engine::EditableDocument& doc) {
  if (!executed_) return make_error(ErrorCode::kInvalidState, "undo before execute");
  auto pages = doc.set_form_text(page_, ref_, before_);
  if (!pages) return std::unexpected(pages.error());
  return pages_change(std::move(*pages));
}

// ---------------------------------------------------------------- 체크·라디오

SetFormCheckedCommand::SetFormCheckedCommand(int page, engine::FieldRef ref, bool checked)
    : page_(page), ref_(std::move(ref)), checked_(checked) {}

Result<engine::ChangeSet> SetFormCheckedCommand::execute(engine::EditableDocument& doc) {
  if (!executed_) {
    auto widget = read_widget(doc, page_, ref_);
    if (!widget) return std::unexpected(widget.error());
    if (widget->type != engine::FieldType::kCheckBox && widget->type != engine::FieldType::kRadio) {
      return make_error(ErrorCode::kInvalidArgument, "not a check box or radio button: " + ref_.name);
    }
    radio_ = widget->type == engine::FieldType::kRadio;
    if (radio_ && !checked_) return make_error(ErrorCode::kUnsupported, "a radio button group cannot be cleared");
    if (widget->checked == checked_) return make_error(ErrorCode::kInvalidArgument, "state unchanged");
    before_checked_ = widget->checked;
    if (radio_) {
      // 같은 필드에서 켜져 있던 위젯: 이 쪽 먼저, 없으면 다른 쪽 (위젯이 여러 쪽에 걸친 그룹)
      const int count = doc.page_count();
      for (int offset = 0; offset < count && previous_page_ < 0; ++offset) {
        const int p = (page_ + offset) % count;
        auto list = doc.form_widgets(p);
        if (!list) return std::unexpected(list.error());
        for (const auto& w : *list) {
          if (w.ref.name == ref_.name && w.checked) {
            previous_page_ = p;
            previous_ref_ = w.ref;
            break;
          }
        }
        if (widget->control_count <= 1) break;
      }
    }
  }
  auto pages = doc.set_form_checked(page_, ref_, checked_);
  if (!pages) return std::unexpected(pages.error());
  executed_ = true;
  return pages_change(std::move(*pages));
}

Result<engine::ChangeSet> SetFormCheckedCommand::undo(engine::EditableDocument& doc) {
  if (!executed_) return make_error(ErrorCode::kInvalidState, "undo before execute");
  if (!radio_) {
    auto pages = doc.set_form_checked(page_, ref_, before_checked_);
    if (!pages) return std::unexpected(pages.error());
    return pages_change(std::move(*pages));
  }
  if (previous_page_ < 0) {
    // 처음 선택 없던 그룹: PDFium 이 라디오 해제를 지원하지 않는다. 실패로 두면 이 단계가 스택에 걸려 그 앞의 편집까지
    // 되돌릴 수 없게 되므로, 문서를 바꾸지 않고 성공한다 (다시 실행도 같은 선택이라 변화 없음). 앱이 undoable() 로 안내
    return engine::ChangeSet{};
  }
  auto pages = doc.set_form_checked(previous_page_, previous_ref_, true);
  if (!pages) return std::unexpected(pages.error());
  return pages_change(std::move(*pages));
}

// ---------------------------------------------------------------- 선택지

SetFormChoiceCommand::SetFormChoiceCommand(int page, engine::FieldRef ref, std::vector<int> indices)
    : page_(page), ref_(std::move(ref)), after_(std::move(indices)) {
  std::sort(after_.begin(), after_.end());
  after_.erase(std::unique(after_.begin(), after_.end()), after_.end());
}

Result<engine::ChangeSet> SetFormChoiceCommand::execute(engine::EditableDocument& doc) {
  if (!executed_) {
    auto widget = read_widget(doc, page_, ref_);
    if (!widget) return std::unexpected(widget.error());
    if (widget->type != engine::FieldType::kComboBox && widget->type != engine::FieldType::kListBox) {
      return make_error(ErrorCode::kInvalidArgument, "not a choice field: " + ref_.name);
    }
    std::vector<int> current = widget->selected;
    std::sort(current.begin(), current.end());
    if (current == after_ && (widget->type == engine::FieldType::kListBox || !widget->selected.empty())) {
      return make_error(ErrorCode::kInvalidArgument, "selection unchanged");
    }
    type_ = widget->type;
    before_selected_ = std::move(current);
    before_value_ = widget->value;
    before_editable_ = widget->editable_combo;
  }
  auto pages = doc.set_form_selection(page_, ref_, after_);
  if (!pages) return std::unexpected(pages.error());
  executed_ = true;
  return pages_change(std::move(*pages));
}

Result<engine::ChangeSet> SetFormChoiceCommand::undo(engine::EditableDocument& doc) {
  if (!executed_) return make_error(ErrorCode::kInvalidState, "undo before execute");
  Result<std::vector<int>> pages;
  if (type_ == engine::FieldType::kComboBox && before_selected_.empty()) {
    // 선택지에 없던 값(직접 입력)이었다: 편집 가능 콤보면 텍스트로, 아니면 되돌릴 수 없다
    if (!before_editable_) return make_error(ErrorCode::kUnsupported, "previous value is not an option");
    pages = doc.set_form_text(page_, ref_, before_value_);
  } else {
    pages = doc.set_form_selection(page_, ref_, before_selected_);
  }
  if (!pages) return std::unexpected(pages.error());
  return pages_change(std::move(*pages));
}

}  // namespace onpdf::edit
