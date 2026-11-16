#include "onpdf/edit/move_pages.h"

#include "onpdf/edit/page_ops.h"

#include <utility>

namespace onpdf::edit {

MovePagesCommand::MovePagesCommand(std::vector<int> pages, int dest)
    : pages_(normalize_pages(std::move(pages))), dest_(dest) {}

Result<engine::ChangeSet> MovePagesCommand::execute(engine::EditableDocument& doc) {
  const int count = doc.page_count();
  if (auto ok = validate_move(count, pages_, dest_); !ok) return std::unexpected(ok.error());
  if (auto moved = doc.move_pages(pages_, dest_); !moved) return std::unexpected(moved.error());
  order_ = simulate_move(count, pages_, dest_);
  return structural_change(doc, dest_);
}

Result<engine::ChangeSet> MovePagesCommand::undo(engine::EditableDocument& doc) {
  if (order_.empty()) return make_error(ErrorCode::kInvalidState, "undo before execute");
  // 자리 old 에는 지금 order_ 에서 old 가 있는 위치(inverse[old])의 쪽이 와야 한다
  if (auto restored = apply_order(doc, invert_order(order_)); !restored) return std::unexpected(restored.error());
  return structural_change(doc, pages_.front());
}

}  // namespace onpdf::edit
