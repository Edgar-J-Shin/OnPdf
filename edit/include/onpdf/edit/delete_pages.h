#pragma once

#include "onpdf/edit/command.h"

#include <vector>

namespace onpdf::edit {

// 쪽 삭제. 지우기 전에 그 쪽들의 사본을 문서 스냅샷으로 떼어 두고, 취소 때 같은 자리에 되돌린다.
// 되돌린 쪽은 새 객체라 그 쪽을 가리키던 북마크·링크는 끊긴 채 남는다 (PRD 수용)
class DeletePagesCommand final : public Command {
 public:
  explicit DeletePagesCommand(std::vector<int> pages);  // 정렬·중복 제거한다
  [[nodiscard]] std::string_view id() const noexcept override { return "delete_pages"; }
  Result<engine::ChangeSet> execute(engine::EditableDocument& doc) override;
  Result<engine::ChangeSet> undo(engine::EditableDocument& doc) override;
  void discard(engine::EditableDocument& doc) noexcept override;
  [[nodiscard]] const std::vector<int>& pages() const noexcept { return pages_; }

 private:
  std::vector<int> pages_;
  engine::SnapshotId snapshot_ = 0;  // execute 때 만들고 discard 때 해제 (워커에서만 접근)
};

}  // namespace onpdf::edit
