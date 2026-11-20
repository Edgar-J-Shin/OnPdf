#pragma once

// 폼 채우기 Command (#51). 위젯은 필드 이름 + 컨트롤 index (engine::FieldRef) 로 가리킨다.
// 이전 값은 첫 실행 때 문서에서 읽어 보관한다. 같은 이름의 위젯은 값을 공유하므로 ChangeSet 은 엔진이 돌려준 쪽 전부

#include "onpdf/core/engine/form.h"
#include "onpdf/edit/command.h"

#include <string>
#include <vector>

namespace onpdf::edit {

// 텍스트 필드·편집 가능 콤보의 값. 값이 같으면 kInvalidArgument (앱이 Command 를 만들지 않는다)
class SetFormTextCommand final : public Command {
 public:
  SetFormTextCommand(int page, engine::FieldRef ref, std::string utf8);
  [[nodiscard]] std::string_view id() const noexcept override { return "form_text"; }
  Result<engine::ChangeSet> execute(engine::EditableDocument& doc) override;
  Result<engine::ChangeSet> undo(engine::EditableDocument& doc) override;
  [[nodiscard]] const std::string& before() const noexcept { return before_; }

 private:
  int page_;
  engine::FieldRef ref_;
  std::string after_;
  std::string before_;
  bool executed_ = false;
};

// 체크박스 켜기·끄기, 라디오 위젯 선택. 라디오의 이전 선택은 같은 필드에서 켜져 있던 위젯 (다른 쪽 포함).
// 처음 선택 없던 라디오 그룹의 첫 선택은 되돌릴 수 없다 (PDFium 이 그룹 해제를 지원하지 않음): undo 는 문서를 바꾸지 않고
// 성공한다 — 실패로 두면 스택이 막혀 그 앞의 편집도 되돌릴 수 없다. 앱은 undoable() 이 거짓이면 안내한다
class SetFormCheckedCommand final : public Command {
 public:
  SetFormCheckedCommand(int page, engine::FieldRef ref, bool checked);
  [[nodiscard]] std::string_view id() const noexcept override { return "form_check"; }
  Result<engine::ChangeSet> execute(engine::EditableDocument& doc) override;
  Result<engine::ChangeSet> undo(engine::EditableDocument& doc) override;
  // 취소할 수 없는 첫 라디오 선택인가 (첫 실행 뒤에 의미가 있다)
  [[nodiscard]] bool undoable() const noexcept { return !radio_ || previous_page_ >= 0; }

 private:
  int page_;
  engine::FieldRef ref_;
  bool checked_;
  bool radio_ = false;
  bool before_checked_ = false;     // 체크박스
  int previous_page_ = -1;          // 라디오: 이전에 켜져 있던 위젯 (없으면 -1)
  engine::FieldRef previous_ref_;
  bool executed_ = false;
};

// 콤보·목록 선택 (선택지 index). 이전 값이 선택지에 없던 편집 가능 콤보는 텍스트로 되돌린다
class SetFormChoiceCommand final : public Command {
 public:
  SetFormChoiceCommand(int page, engine::FieldRef ref, std::vector<int> indices);
  [[nodiscard]] std::string_view id() const noexcept override { return "form_choice"; }
  Result<engine::ChangeSet> execute(engine::EditableDocument& doc) override;
  Result<engine::ChangeSet> undo(engine::EditableDocument& doc) override;

 private:
  int page_;
  engine::FieldRef ref_;
  std::vector<int> after_;
  std::vector<int> before_selected_;
  std::string before_value_;
  bool before_editable_ = false;
  engine::FieldType type_ = engine::FieldType::kUnknown;
  bool executed_ = false;
};

}  // namespace onpdf::edit
