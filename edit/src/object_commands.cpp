#include "onpdf/edit/object_commands.h"

#include <utility>

namespace onpdf::edit {
namespace {

Result<engine::ChangeSet> page_change(int page) {
  engine::ChangeSet change;
  change.pages = {page};
  return change;
}

// "#obj<n>" 은 마크를 붙여 안정된 id 로 바꾼다 (첫 실행에서 한 번)
Result<void> stabilize(engine::EditableDocument& doc, int page, std::vector<std::string>& ids) {
  for (auto& id : ids) {
    if (!id.starts_with("#obj")) continue;
    auto marked = doc.mark_object(page, id);
    if (!marked) return std::unexpected(marked.error());
    id = std::move(*marked);
  }
  return {};
}

}  // namespace

// ---------------------------------------------------------------- Add image

AddImageCommand::AddImageCommand(int page, engine::ImageSpec spec)
    : page_(page), spec_(std::move(spec)) {}

Result<engine::ChangeSet> AddImageCommand::execute(engine::EditableDocument& doc) {
  if (detached_ != 0) {  // 재실행: 떼어 둔 같은 객체를 다시 붙인다 (픽셀을 다시 넣지 않는다)
    if (auto reattached = doc.reattach_objects(detached_); !reattached)
      return std::unexpected(reattached.error());
    detached_ = 0;
    return page_change(page_);
  }
  if (executed_) return make_error(ErrorCode::kInvalidState, "already executed");
  auto added = doc.add_image_object(page_, spec_);
  if (!added) return std::unexpected(added.error());
  object_id_ = std::move(*added);
  spec_.source = engine::ImagePixels{};  // 픽셀·JPEG 데이터는 더 필요 없다 (메인 스레드 메모리)
  executed_ = true;
  return page_change(page_);
}

Result<engine::ChangeSet> AddImageCommand::undo(engine::EditableDocument& doc) {
  if (!executed_ || detached_ != 0)
    return make_error(ErrorCode::kInvalidState, "undo before execute");
  auto detached = doc.detach_objects(page_, {object_id_});
  if (!detached) return std::unexpected(detached.error());
  detached_ = *detached;
  return page_change(page_);
}

void AddImageCommand::discard(engine::EditableDocument& doc) noexcept {
  if (detached_ != 0) doc.drop_detached(detached_);
  detached_ = 0;
}

// ---------------------------------------------------------------- Move

MoveObjectsCommand::MoveObjectsCommand(int page, std::vector<std::string> ids, Point delta,
                                       bool keyboard)
    : page_(page), ids_(std::move(ids)), delta_(delta), keyboard_(keyboard) {}

Result<engine::ChangeSet> MoveObjectsCommand::execute(engine::EditableDocument& doc) {
  if (ids_.empty()) return make_error(ErrorCode::kInvalidArgument, "no ids");
  if (!executed_) {
    if (auto stable = stabilize(doc, page_, ids_); !stable) return std::unexpected(stable.error());
  }
  if (auto moved = doc.move_page_objects(page_, ids_, delta_); !moved)
    return std::unexpected(moved.error());
  executed_ = true;
  return page_change(page_);
}

Result<engine::ChangeSet> MoveObjectsCommand::undo(engine::EditableDocument& doc) {
  if (!executed_) return make_error(ErrorCode::kInvalidState, "undo before execute");
  if (auto moved = doc.move_page_objects(page_, ids_, {-delta_.x, -delta_.y}); !moved)
    return std::unexpected(moved.error());
  return page_change(page_);
}

bool MoveObjectsCommand::merge_with(const Command& next) {
  const auto* other = dynamic_cast<const MoveObjectsCommand*>(&next);
  if (other == nullptr || !keyboard_ || !other->keyboard_ || other->page_ != page_ ||
      other->ids_ != ids_)
    return false;
  delta_.x += other->delta_.x;
  delta_.y += other->delta_.y;
  return true;
}

// ---------------------------------------------------------------- Resize

ResizeObjectCommand::ResizeObjectCommand(int page, std::string id, Rect before, Rect after)
    : page_(page), object_id_(std::move(id)), before_(before), after_(after) {}

Result<engine::ChangeSet> ResizeObjectCommand::execute(engine::EditableDocument& doc) {
  if (!executed_ && object_id_.starts_with("#obj")) {
    auto marked = doc.mark_object(page_, object_id_);
    if (!marked) return std::unexpected(marked.error());
    object_id_ = std::move(*marked);
  }
  if (auto set = doc.set_object_rect(page_, object_id_, after_); !set)
    return std::unexpected(set.error());
  executed_ = true;
  return page_change(page_);
}

Result<engine::ChangeSet> ResizeObjectCommand::undo(engine::EditableDocument& doc) {
  if (!executed_) return make_error(ErrorCode::kInvalidState, "undo before execute");
  if (auto set = doc.set_object_rect(page_, object_id_, before_); !set)
    return std::unexpected(set.error());
  return page_change(page_);
}

// ---------------------------------------------------------------- Remove

RemoveObjectsCommand::RemoveObjectsCommand(int page, std::vector<std::string> ids)
    : page_(page), ids_(std::move(ids)) {}

Result<engine::ChangeSet> RemoveObjectsCommand::execute(engine::EditableDocument& doc) {
  if (ids_.empty()) return make_error(ErrorCode::kInvalidArgument, "no ids");
  if (!executed_) {
    if (auto stable = stabilize(doc, page_, ids_); !stable) return std::unexpected(stable.error());
  }
  auto detached = doc.detach_objects(page_, ids_);
  if (!detached) return std::unexpected(detached.error());
  detached_ = *detached;
  executed_ = true;
  return page_change(page_);
}

Result<engine::ChangeSet> RemoveObjectsCommand::undo(engine::EditableDocument& doc) {
  if (!executed_ || detached_ == 0)
    return make_error(ErrorCode::kInvalidState, "undo before execute");
  if (auto reattached = doc.reattach_objects(detached_); !reattached)
    return std::unexpected(reattached.error());
  detached_ = 0;
  return page_change(page_);
}

void RemoveObjectsCommand::discard(engine::EditableDocument& doc) noexcept {
  if (detached_ != 0) doc.drop_detached(detached_);
  detached_ = 0;
}

}  // namespace onpdf::edit
