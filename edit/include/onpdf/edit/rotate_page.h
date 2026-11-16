#pragma once

#include "onpdf/edit/command.h"

#include <vector>

namespace onpdf::edit {

// 쪽 회전 (시계 방향 quarter_turns). 여러 쪽을 한 번에 돌리면 한 실행 취소 단위다
class RotatePageCommand final : public Command {
 public:
  RotatePageCommand(int page, int quarter_turns);
  RotatePageCommand(std::vector<int> pages, int quarter_turns);  // 정렬·중복 제거한다
  [[nodiscard]] std::string_view id() const noexcept override { return "rotate_page"; }
  Result<engine::ChangeSet> execute(engine::EditableDocument& doc) override;
  Result<engine::ChangeSet> undo(engine::EditableDocument& doc) override;
  [[nodiscard]] int page() const noexcept { return pages_.empty() ? -1 : pages_.front(); }
  [[nodiscard]] const std::vector<int>& pages() const noexcept { return pages_; }

 private:
  Result<engine::ChangeSet> apply(engine::EditableDocument& doc, const std::vector<int>& rotations);
  std::vector<int> pages_;
  int quarter_turns_;
  std::vector<int> previous_;  // execute 때 기록 (워커에서만 접근). 비어 있으면 아직 실행 전
};

}  // namespace onpdf::edit
