#include "onpdf/edit/annotation_commands.h"

#include <algorithm>
#include <utility>

namespace onpdf::edit {
namespace {

engine::ChangeSet page_change(std::vector<int> pages) {
  std::sort(pages.begin(), pages.end());
  pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
  engine::ChangeSet change;
  change.pages = std::move(pages);
  return change;
}

}  // namespace

std::string_view add_command_id(engine::AnnotationKind kind) noexcept {
  switch (kind) {
    case engine::AnnotationKind::kHighlight: return "add_highlight";
    case engine::AnnotationKind::kUnderline: return "add_underline";
    case engine::AnnotationKind::kStrikeOut: return "add_strikeout";
    case engine::AnnotationKind::kSquiggly: return "add_squiggly";
    case engine::AnnotationKind::kText: return "add_note";
    case engine::AnnotationKind::kInk: return "add_ink";
    default: return "add_annotation";
  }
}

// ---------------------------------------------------------------- Add

AddAnnotationsCommand::AddAnnotationsCommand(std::vector<std::pair<int, engine::AnnotationSpec>> items)
    : items_(std::move(items)), id_(items_.empty() ? "add_annotation" : add_command_id(items_.front().second.kind)) {}

Result<engine::ChangeSet> AddAnnotationsCommand::execute(engine::EditableDocument& doc) {
  if (items_.empty()) return make_error(ErrorCode::kInvalidArgument, "no annotations to add");
  std::vector<int> pages;
  for (std::size_t i = 0; i < items_.size(); ++i) {
    auto& [page, spec] = items_[i];
    auto added = doc.add_annotation(page, spec);
    if (!added) {
      // 보상: 이 실행에서 만든 것을 지운다 (다중 쪽 마크업이 중간에 실패해도 반만 남지 않게)
      for (std::size_t k = 0; k < i; ++k) (void)doc.remove_annotation(items_[k].first, items_[k].second.id);
      return std::unexpected(added.error());
    }
    spec.id = *added;  // 재실행 때 같은 NM
    pages.push_back(page);
  }
  executed_ = true;
  return page_change(std::move(pages));
}

Result<engine::ChangeSet> AddAnnotationsCommand::undo(engine::EditableDocument& doc) {
  if (!executed_) return make_error(ErrorCode::kInvalidState, "undo before execute");
  std::vector<int> pages;
  for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
    if (auto removed = doc.remove_annotation(it->first, it->second.id); !removed) return std::unexpected(removed.error());
    pages.push_back(it->first);
  }
  return page_change(std::move(pages));
}

// ---------------------------------------------------------------- Remove

RemoveAnnotationCommand::RemoveAnnotationCommand(int page, std::string id) : page_(page), annotation_id_(std::move(id)) {}

Result<engine::ChangeSet> RemoveAnnotationCommand::execute(engine::EditableDocument& doc) {
  auto snap = doc.snapshot_annotation(page_, annotation_id_);
  if (!snap) return std::unexpected(snap.error());
  snapshot_ = std::move(*snap);
  annotation_id_ = snapshot_.id;  // "#n" 이었다면 이제 NM
  if (auto removed = doc.remove_annotation(page_, annotation_id_); !removed) return std::unexpected(removed.error());
  executed_ = true;
  return page_change({page_});
}

Result<engine::ChangeSet> RemoveAnnotationCommand::undo(engine::EditableDocument& doc) {
  if (!executed_) return make_error(ErrorCode::kInvalidState, "undo before execute");
  if (auto restored = doc.restore_annotation(page_, snapshot_); !restored) return std::unexpected(restored.error());
  return page_change({page_});
}

// ---------------------------------------------------------------- Color

SetAnnotationColorCommand::SetAnnotationColorCommand(int page, std::string id, engine::Color color)
    : page_(page), annotation_id_(std::move(id)), color_(color) {}

Result<engine::ChangeSet> SetAnnotationColorCommand::execute(engine::EditableDocument& doc) {
  auto before = doc.snapshot_annotation(page_, annotation_id_);
  if (!before) return std::unexpected(before.error());
  before_ = std::move(*before);
  annotation_id_ = before_.id;
  if (auto set = doc.set_annotation_color(page_, annotation_id_, color_); !set) return std::unexpected(set.error());
  executed_ = true;
  return page_change({page_});
}

Result<engine::ChangeSet> SetAnnotationColorCommand::undo(engine::EditableDocument& doc) {
  if (!executed_) return make_error(ErrorCode::kInvalidState, "undo before execute");
  // 이전 색으로 (모양은 PDFium 이 다시 그린다)
  if (auto set = doc.set_annotation_color(page_, annotation_id_, before_.color); !set) return std::unexpected(set.error());
  return page_change({page_});
}

// ---------------------------------------------------------------- Contents

SetAnnotationContentsCommand::SetAnnotationContentsCommand(int page, std::string id, std::string contents_utf8)
    : page_(page), annotation_id_(std::move(id)), contents_(std::move(contents_utf8)) {}

Result<engine::ChangeSet> SetAnnotationContentsCommand::execute(engine::EditableDocument& doc) {
  auto before = doc.snapshot_annotation(page_, annotation_id_);
  if (!before) return std::unexpected(before.error());
  before_ = before->contents;
  annotation_id_ = before->id;
  if (auto set = doc.set_annotation_contents(page_, annotation_id_, contents_); !set) return std::unexpected(set.error());
  executed_ = true;
  return page_change({page_});
}

Result<engine::ChangeSet> SetAnnotationContentsCommand::undo(engine::EditableDocument& doc) {
  if (!executed_) return make_error(ErrorCode::kInvalidState, "undo before execute");
  if (auto set = doc.set_annotation_contents(page_, annotation_id_, before_); !set) return std::unexpected(set.error());
  return page_change({page_});
}

}  // namespace onpdf::edit
