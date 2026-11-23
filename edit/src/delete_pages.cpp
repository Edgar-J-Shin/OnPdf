#include "onpdf/edit/delete_pages.h"

#include "onpdf/edit/page_ops.h"

#include <utility>

namespace onpdf::edit {

DeletePagesCommand::DeletePagesCommand(std::vector<int> pages)
    : pages_(normalize_pages(std::move(pages))) {}

Result<engine::ChangeSet> DeletePagesCommand::execute(engine::EditableDocument& doc) {
  if (pages_.empty()) return make_error(ErrorCode::kInvalidArgument, "delete: no pages");
  if (static_cast<int>(pages_.size()) >= doc.page_count()) {
    return make_error(ErrorCode::kInvalidArgument, "cannot delete all pages");
  }
  // 다시 실행이면 이전 스냅샷을 버리고 지금 상태로 새로 뜬다 (그 사이 다른 편집이 이 쪽들을 바꿨을
  // 수 있다)
  if (snapshot_ != 0) doc.drop_snapshot(snapshot_);
  snapshot_ = 0;
  auto snapshot = doc.copy_pages_to_snapshot(pages_);
  if (!snapshot) return std::unexpected(snapshot.error());
  snapshot_ = *snapshot;
  if (auto deleted = doc.delete_pages(pages_); !deleted) {
    discard(doc);  // 실패한 실행은 스택에 오르지 않으므로 여기서 해제한다
    return std::unexpected(deleted.error());
  }
  return structural_change(doc, pages_.front());
}

Result<engine::ChangeSet> DeletePagesCommand::undo(engine::EditableDocument& doc) {
  if (snapshot_ == 0) return make_error(ErrorCode::kInvalidState, "undo before execute");
  // 오름차순으로 되돌리면 앞서 되돌린 쪽이 이미 자리를 차지해 각 원래 번호가 그대로 맞는다
  for (std::size_t k = 0; k < pages_.size(); ++k) {
    auto inserted = doc.insert_from_snapshot(snapshot_, {static_cast<int>(k)}, pages_[k]);
    if (!inserted) return std::unexpected(inserted.error());
  }
  return structural_change(doc, pages_.front(), pages_);
}

void DeletePagesCommand::discard(engine::EditableDocument& doc) noexcept {
  if (snapshot_ != 0) doc.drop_snapshot(snapshot_);
  snapshot_ = 0;
}

}  // namespace onpdf::edit
