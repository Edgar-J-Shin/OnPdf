#pragma once

#include "onpdf/edit/command.h"

#include <vector>

namespace onpdf::edit {

// 쪽 이동: pages(오름차순 블록)를 dest 자리(블록을 뺀 뒤의 삽입 위치)로. 쪽 객체는 그대로라
// 목차·링크가 유지된다. 취소는 역순열을 단일 쪽 이동의 열로 적용한다 (비연속 선택도 정확히
// 되돌린다)
class MovePagesCommand final : public Command {
 public:
  MovePagesCommand(std::vector<int> pages, int dest);  // 정렬·중복 제거한다
  [[nodiscard]] std::string_view id() const noexcept override { return "move_pages"; }
  Result<engine::ChangeSet> execute(engine::EditableDocument& doc) override;
  Result<engine::ChangeSet> undo(engine::EditableDocument& doc) override;
  [[nodiscard]] const std::vector<int>& pages() const noexcept { return pages_; }
  [[nodiscard]] int dest() const noexcept { return dest_; }

 private:
  std::vector<int> pages_;
  int dest_;
  std::vector<int> order_;  // execute 뒤: order_[new] = old (워커에서만 접근)
};

}  // namespace onpdf::edit
