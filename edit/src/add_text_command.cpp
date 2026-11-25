#include "onpdf/edit/add_text_command.h"

#include "onpdf/edit/text_box_commands.h"
#include "onpdf/edit/text_layout.h"

#include <algorithm>
#include <set>

namespace onpdf::edit {
namespace {

Result<engine::ChangeSet> page_change(int page) {
  engine::ChangeSet change;
  change.pages = {page};
  return change;
}

}  // namespace

AddTextCommand::AddTextCommand(TextBoxSpec spec) : spec_(std::move(spec)) {}

Result<engine::ChangeSet> AddTextCommand::execute(engine::EditableDocument& doc) {
  // 배치·서브셋·ToUnicode 는 텍스트 상자 Command 들과 공용 (text_box_commands)
  auto spec = build_text_objects_spec(spec_, box_, executed_ ? ids_ : std::vector<std::string>{});
  if (!spec) return std::unexpected(spec.error());
  auto added = doc.add_text_objects(spec_.page, *spec);
  if (!added) return std::unexpected(added.error());
  ids_ = std::move(added->ids);
  box_ = added->box;
  executed_ = true;
  return page_change(spec_.page);
}

Result<engine::ChangeSet> AddTextCommand::undo(engine::EditableDocument& doc) {
  if (!executed_ || ids_.empty())
    return make_error(ErrorCode::kInvalidState, "undo before execute");
  if (auto removed = doc.remove_text_objects(spec_.page, ids_); !removed)
    return std::unexpected(removed.error());
  return page_change(spec_.page);
}

}  // namespace onpdf::edit
