#pragma once

#include "onpdf/edit/command.h"

namespace onpdf::edit {

// 쪽 회전: 시계 방향 quarter_turns (1 = 90°, 3 = 반시계 90°)
class RotatePageCommand final : public Command {
 public:
  RotatePageCommand(int page, int quarter_turns);
  [[nodiscard]] std::string_view id() const noexcept override { return "rotate_page"; }
  Result<engine::ChangeSet> execute(engine::EditableDocument& doc) override;
  Result<engine::ChangeSet> undo(engine::EditableDocument& doc) override;
  [[nodiscard]] int page() const noexcept { return page_; }

 private:
  Result<engine::ChangeSet> apply(engine::EditableDocument& doc, int rotation);
  int page_;
  int quarter_turns_;
  int previous_ = -1;  // execute 때 기록 (워커에서만 접근)
};

}  // namespace onpdf::edit
