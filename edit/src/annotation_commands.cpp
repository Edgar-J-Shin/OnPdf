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
    case engine::AnnotationKind::kSquare: return "add_square";
    case engine::AnnotationKind::kCircle: return "add_circle";
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

// ---------------------------------------------------------------- Border width

SetAnnotationBorderWidthCommand::SetAnnotationBorderWidthCommand(int page, std::string id, double width)
    : page_(page), annotation_id_(std::move(id)), width_(width) {}

Result<engine::ChangeSet> SetAnnotationBorderWidthCommand::execute(engine::EditableDocument& doc) {
  if (!(width_ > 0.0)) return make_error(ErrorCode::kInvalidArgument, "border width must be positive");
  auto before = doc.snapshot_annotation(page_, annotation_id_);
  if (!before) return std::unexpected(before.error());
  if (!engine::has_border_width(before->kind)) return make_error(ErrorCode::kInvalidArgument, "annotation has no border width");
  // 재실행에서는 첫 실행의 스냅샷(원래 값)을 유지한다 — 재생성 뒤의 스냅샷은 이미 새 폭이다
  if (!executed_) before_ = *before;
  annotation_id_ = before_.id;
  if (before->has_bs) {
    // /BS /W 가 /Border 를 덮으므로 /BS 없이 같은 주석을 다시 만든다 (삭제 취소와 같은 경로·같은 한계)
    engine::AnnotationSnapshot after = *before;
    after.border_width = static_cast<float>(width_);
    after.has_border = true;
    after.has_bs = false;
    if (auto removed = doc.remove_annotation(page_, annotation_id_); !removed) return std::unexpected(removed.error());
    if (auto restored = doc.restore_annotation(page_, after); !restored) {
      (void)doc.restore_annotation(page_, before_);  // 보상: 지운 주석을 원래 스냅샷으로 되살린다 (실패해도 오류는 원인 그대로)
      return std::unexpected(restored.error());
    }
    recreated_ = true;
  } else {
    if (auto set = doc.set_annotation_border_width(page_, annotation_id_, width_); !set) return std::unexpected(set.error());
  }
  executed_ = true;
  return page_change({page_});
}

Result<engine::ChangeSet> SetAnnotationBorderWidthCommand::undo(engine::EditableDocument& doc) {
  if (!executed_) return make_error(ErrorCode::kInvalidState, "undo before execute");
  if (recreated_) {
    // 원래 /BS 는 되살릴 수 없다: 스냅샷(/Border 값 또는 없음)으로 다시 만든다
    if (auto removed = doc.remove_annotation(page_, annotation_id_); !removed) return std::unexpected(removed.error());
    if (auto restored = doc.restore_annotation(page_, before_); !restored) {
      engine::AnnotationSnapshot current = before_;  // 보상: 실행 뒤 상태(새 폭)로라도 되살린다
      current.border_width = static_cast<float>(width_);
      current.has_border = true;
      current.has_bs = false;
      (void)doc.restore_annotation(page_, current);
      return std::unexpected(restored.error());
    }
    return page_change({page_});
  }
  const double previous = before_.has_border ? before_.border_width : 1.0;  // PDF 기본 폭
  if (auto set = doc.set_annotation_border_width(page_, annotation_id_, previous); !set) return std::unexpected(set.error());
  return page_change({page_});
}

// ---------------------------------------------------------------- Opacity

SetAnnotationOpacityCommand::SetAnnotationOpacityCommand(int page, std::string id, std::uint8_t alpha)
    : page_(page), annotation_id_(std::move(id)), alpha_(alpha) {}

Result<engine::ChangeSet> SetAnnotationOpacityCommand::execute(engine::EditableDocument& doc) {
  auto before = doc.snapshot_annotation(page_, annotation_id_);
  if (!before) return std::unexpected(before.error());
  if (!before->has_color) return make_error(ErrorCode::kInvalidArgument, "annotation has no color");
  if (!executed_) before_ = *before;
  annotation_id_ = before_.id;
  engine::Color color = before_.color;
  color.a = alpha_;
  if (auto set = doc.set_annotation_color(page_, annotation_id_, color); !set) return std::unexpected(set.error());
  executed_ = true;
  return page_change({page_});
}

Result<engine::ChangeSet> SetAnnotationOpacityCommand::undo(engine::EditableDocument& doc) {
  if (!executed_) return make_error(ErrorCode::kInvalidState, "undo before execute");
  if (auto set = doc.set_annotation_color(page_, annotation_id_, before_.color); !set) return std::unexpected(set.error());
  return page_change({page_});
}

}  // namespace onpdf::edit
