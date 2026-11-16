#include "onpdf/edit/insert_pages.h"

#include "onpdf/edit/page_ops.h"

#include <utility>

namespace onpdf::edit {

InsertPagesCommand::InsertPagesCommand(std::filesystem::path source, std::string password,
                                       std::vector<int> source_pages, int at)
    : source_(std::move(source)),
      password_(std::move(password)),
      source_pages_(normalize_pages(std::move(source_pages))),
      at_(at) {}

Result<engine::ChangeSet> InsertPagesCommand::execute(engine::EditableDocument& doc) {
  if (snapshot_ == 0) {
    auto opened = doc.open_snapshot(source_, password_);
    if (!opened) return std::unexpected(opened.error());
    snapshot_ = *opened;
  }
  // 아래 실패는 다시 실행해도 같으므로(권한·범위) 소스를 바로 버린다. 실패한 실행은 스택에 오르지 않는다
  const auto fail = [&](Error error) -> Result<engine::ChangeSet> {
    discard(doc);
    return std::unexpected(std::move(error));
  };
  auto info = doc.snapshot_info(snapshot_);
  if (!info) return fail(info.error());
  if (!info->permissions.extract) return fail(Error{ErrorCode::kPermissionDenied, "source document forbids extraction"});
  if (info->page_count <= 0) return fail(Error{ErrorCode::kInvalidArgument, "source document has no pages"});
  for (int p : source_pages_) {
    if (p < 0 || p >= info->page_count) {
      return fail(Error{ErrorCode::kInvalidArgument, "source page out of range: " + std::to_string(p)});
    }
  }
  if (at_ < 0 || at_ > doc.page_count()) {
    return fail(Error{ErrorCode::kInvalidArgument, "insert position out of range: " + std::to_string(at_)});
  }
  if (auto inserted = doc.insert_from_snapshot(snapshot_, source_pages_, at_); !inserted) return fail(inserted.error());
  inserted_ = source_pages_.empty() ? info->page_count : static_cast<int>(source_pages_.size());
  std::vector<int> block;
  for (int i = 0; i < inserted_; ++i) block.push_back(at_ + i);
  return structural_change(doc, at_, std::move(block));
}

Result<engine::ChangeSet> InsertPagesCommand::undo(engine::EditableDocument& doc) {
  if (inserted_ <= 0) return make_error(ErrorCode::kInvalidState, "undo before execute");
  std::vector<int> range;
  range.reserve(static_cast<std::size_t>(inserted_));
  for (int i = 0; i < inserted_; ++i) range.push_back(at_ + i);
  if (auto deleted = doc.delete_pages(range); !deleted) return std::unexpected(deleted.error());
  return structural_change(doc, at_);
}

void InsertPagesCommand::discard(engine::EditableDocument& doc) noexcept {
  if (snapshot_ != 0) doc.drop_snapshot(snapshot_);
  snapshot_ = 0;
}

}  // namespace onpdf::edit
