#include "onpdf/edit/rotate_page.h"

namespace onpdf::edit {

RotatePageCommand::RotatePageCommand(int page, int quarter_turns)
    : page_(page), quarter_turns_(((quarter_turns % 4) + 4) % 4) {}

Result<engine::ChangeSet> RotatePageCommand::apply(engine::EditableDocument& doc, int rotation) {
  if (auto set = doc.set_page_rotation(page_, rotation); !set) return std::unexpected(set.error());
  auto size = doc.page_size(page_);
  if (!size) return std::unexpected(size.error());
  engine::ChangeSet change;
  change.pages.push_back(page_);
  change.new_sizes.emplace_back(page_, *size);
  return change;
}

Result<engine::ChangeSet> RotatePageCommand::execute(engine::EditableDocument& doc) {
  auto current = doc.page_rotation(page_);
  if (!current) return std::unexpected(current.error());
  previous_ = *current;
  return apply(doc, (*current + quarter_turns_) % 4);
}

Result<engine::ChangeSet> RotatePageCommand::undo(engine::EditableDocument& doc) {
  if (previous_ < 0) return make_error(ErrorCode::kInvalidState, "undo before execute");
  return apply(doc, previous_);
}

}  // namespace onpdf::edit
