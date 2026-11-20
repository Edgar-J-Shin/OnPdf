#include "onpdf/edit/text_line_commands.h"

#include "onpdf/core/engine/text_object.h"
#include "onpdf/edit/add_text_command.h"
#include "onpdf/edit/text_box_commands.h"

#include <algorithm>

namespace onpdf::edit {
namespace {

Result<engine::ChangeSet> page_change(int page) {
  engine::ChangeSet change;
  change.pages = {page};
  return change;
}

constexpr std::string_view kBoxPrefix = "onpdf-box-";

struct LineGeometry {
  Point origin;  // 상자 왼쪽 위 (쪽 포인트)
  double size = 0.0;
  engine::Color color;
  std::string text;
};

// 줄(또는 OnPdf 상자)의 자리·크기·색·원문
Result<LineGeometry> geometry_of(engine::EditableDocument& doc, int page, std::string_view line_id) {
  LineGeometry out;
  if (line_id.starts_with(kBoxPrefix)) {
    auto objects = doc.text_objects(page);
    if (!objects) return std::unexpected(objects.error());
    bool found = false;
    for (const auto& o : *objects) {
      if (o.box != line_id) continue;
      if (!found) {
        out.origin = {o.bbox.x, o.bbox.y};
        out.size = o.font_size;
        out.color = o.color;
        out.text = o.box_text.empty() ? o.text : o.box_text;
        found = true;
      } else if (o.box_text.empty()) {
        out.text += "\n" + o.text;
      }
    }
    if (!found) return make_error(ErrorCode::kInvalidArgument, "text box not found: " + std::string(line_id));
    return out;
  }
  auto lines = doc.text_lines(page);
  if (!lines) return std::unexpected(lines.error());
  for (const auto& l : *lines) {
    if (l.id != line_id) continue;
    out.origin = {l.bbox.x, l.bbox.y};
    out.size = l.font_size;
    out.color = l.color;
    out.text = l.text;
    return out;
  }
  return make_error(ErrorCode::kInvalidArgument, "line not found: " + std::string(line_id));
}

}  // namespace

ReplaceTextLineCommand::ReplaceTextLineCommand(int page, std::string line_id, std::string utf8,
                                               std::shared_ptr<const FontFace> fallback_face)
    : page_(page), line_id_(std::move(line_id)), utf8_(std::move(utf8)), face_(std::move(fallback_face)),
      onpdf_box_(line_id_.starts_with(kBoxPrefix)) {}

Result<engine::ChangeSet> ReplaceTextLineCommand::execute(engine::EditableDocument& doc) {
  if (utf8_.empty()) return make_error(ErrorCode::kInvalidArgument, "empty text");
  // 다시 실행: 같은 줄 id 로 재수술 (복원된 바이트에서 id 가 같다). OnPdf 상자 경로는 상자 id 가 그대로다
  const std::string_view target = (executed_ && onpdf_box_) ? std::string_view(box_) : std::string_view(line_id_);
  auto geometry = geometry_of(doc, page_, target);
  if (!geometry) return std::unexpected(geometry.error());
  if (!executed_) previous_utf8_ = geometry->text;
  engine::LineReplacement replacement;
  replacement.utf8 = utf8_;
  auto covers = doc.line_font_covers(page_, target, utf8_);
  if (!covers) return std::unexpected(covers.error());
  if (!*covers) {
    if (face_ == nullptr) return make_error(ErrorCode::kUnsupported, "original font cannot draw the text");
    TextBoxSpec spec;
    spec.page = page_;
    spec.origin = geometry->origin;
    spec.size = geometry->size > 0.0 ? geometry->size : 12.0;
    spec.color = geometry->color;
    spec.utf8 = utf8_;
    spec.face = face_;
    auto built = build_text_objects_spec(spec, onpdf_box_ ? std::string(target) : engine::make_text_box_id(), {});
    if (!built) return std::unexpected(built.error());
    replacement.fallback = std::move(*built);
  }
  if (before_ != 0) {
    doc.drop_document_state(before_);
    before_ = 0;
  }
  auto result = doc.replace_text_line(page_, target, replacement);
  if (!result) return std::unexpected(result.error());
  before_ = result->before;
  used_fallback_ = result->used_fallback;
  box_ = result->box;
  ids_ = result->ids;
  executed_ = true;
  return page_change(page_);
}

Result<engine::ChangeSet> ReplaceTextLineCommand::undo(engine::EditableDocument& doc) {
  if (!executed_) return make_error(ErrorCode::kInvalidState, "undo before execute");
  if (before_ != 0) {
    if (auto restored = doc.restore_document_state(before_); !restored) return std::unexpected(restored.error());
    return page_change(page_);
  }
  // OnPdf 상자: 수술이 없었다 → 이전 글로 다시 교체 (원본 글꼴 유지)
  engine::LineReplacement replacement;
  replacement.utf8 = previous_utf8_;
  if (auto geometry = geometry_of(doc, page_, box_); geometry && face_ != nullptr) {
    TextBoxSpec spec;
    spec.page = page_;
    spec.origin = geometry->origin;
    spec.size = geometry->size > 0.0 ? geometry->size : 12.0;
    spec.color = geometry->color;
    spec.utf8 = previous_utf8_;
    spec.face = face_;
    if (auto built = build_text_objects_spec(spec, box_, {})) replacement.fallback = std::move(*built);
  }
  auto result = doc.replace_text_line(page_, box_, replacement);
  if (!result) return std::unexpected(result.error());
  return page_change(page_);
}

void ReplaceTextLineCommand::discard(engine::EditableDocument& doc) noexcept {
  if (before_ != 0) doc.drop_document_state(before_);
  before_ = 0;
}

RemoveTextLineCommand::RemoveTextLineCommand(int page, std::string line_id) : page_(page), line_id_(std::move(line_id)) {}

Result<engine::ChangeSet> RemoveTextLineCommand::execute(engine::EditableDocument& doc) {
  if (before_ != 0) {
    doc.drop_document_state(before_);
    before_ = 0;
  }
  auto result = doc.remove_text_line(page_, line_id_);
  if (!result) return std::unexpected(result.error());
  before_ = result->before;
  executed_ = true;
  return page_change(page_);
}

Result<engine::ChangeSet> RemoveTextLineCommand::undo(engine::EditableDocument& doc) {
  if (!executed_ || before_ == 0) return make_error(ErrorCode::kInvalidState, "undo before execute");
  if (auto restored = doc.restore_document_state(before_); !restored) return std::unexpected(restored.error());
  return page_change(page_);
}

void RemoveTextLineCommand::discard(engine::EditableDocument& doc) noexcept {
  if (before_ != 0) doc.drop_document_state(before_);
  before_ = 0;
}

}  // namespace onpdf::edit
