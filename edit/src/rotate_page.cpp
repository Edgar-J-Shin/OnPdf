#include "onpdf/edit/rotate_page.h"

#include "onpdf/edit/page_ops.h"

#include <utility>

namespace onpdf::edit {

RotatePageCommand::RotatePageCommand(int page, int quarter_turns)
    : RotatePageCommand(std::vector<int>{page}, quarter_turns) {}

RotatePageCommand::RotatePageCommand(std::vector<int> pages, int quarter_turns)
    : pages_(normalize_pages(std::move(pages))), quarter_turns_(((quarter_turns % 4) + 4) % 4) {}

Result<engine::ChangeSet> RotatePageCommand::apply(engine::EditableDocument& doc,
                                                   const std::vector<int>& rotations) {
  engine::ChangeSet change;
  for (std::size_t i = 0; i < pages_.size(); ++i) {
    const int page = pages_[i];
    if (auto set = doc.set_page_rotation(page, rotations[i]); !set)
      return std::unexpected(set.error());
    auto size = doc.page_size(page);
    if (!size) return std::unexpected(size.error());
    change.pages.push_back(page);
    change.new_sizes.emplace_back(page, *size);
  }
  return change;
}

Result<engine::ChangeSet> RotatePageCommand::execute(engine::EditableDocument& doc) {
  if (pages_.empty()) return make_error(ErrorCode::kInvalidArgument, "rotate: no pages");
  // 먼저 전부 읽어 잘못된 쪽이 있으면 아무것도 바꾸지 않는다
  std::vector<int> current;
  current.reserve(pages_.size());
  for (int page : pages_) {
    auto rotation = doc.page_rotation(page);
    if (!rotation) return std::unexpected(rotation.error());
    current.push_back(*rotation);
  }
  std::vector<int> next(current.size());
  for (std::size_t i = 0; i < current.size(); ++i) next[i] = (current[i] + quarter_turns_) % 4;
  previous_ = std::move(current);
  return apply(doc, next);
}

Result<engine::ChangeSet> RotatePageCommand::undo(engine::EditableDocument& doc) {
  if (previous_.empty()) return make_error(ErrorCode::kInvalidState, "undo before execute");
  return apply(doc, previous_);
}

}  // namespace onpdf::edit
