#include "onpdf/edit/text_box_commands.h"

#include "onpdf/edit/text_layout.h"

#include <algorithm>
#include <utility>

namespace onpdf::edit {
namespace {

Result<engine::ChangeSet> page_change(int page) {
  engine::ChangeSet change;
  change.pages = {page};
  return change;
}

bool visible(const LaidOutLine& line) {
  return std::any_of(line.glyphs.begin(), line.glyphs.end(), [](const ShapedGlyph& g) {
    return !std::all_of(g.text.begin(), g.text.end(), is_layout_whitespace);
  });
}

// 줄 id 를 순서대로 재사용하고 모자라면 새로 만든다, 남으면 버린다 (재실행이 결정적이도록 Command 가 보관)
std::vector<std::string> reuse_ids(const std::vector<std::string>& previous, std::size_t needed) {
  std::vector<std::string> ids(previous.begin(), previous.begin() + static_cast<std::ptrdiff_t>(std::min(previous.size(), needed)));
  while (ids.size() < needed) ids.push_back(engine::make_text_object_id());  // 엔진과 같은 형식 (ids 는 전부 있거나 전부 없어야 한다)
  return ids;
}

}  // namespace

Result<engine::TextObjectsSpec> build_text_objects_spec(const TextBoxSpec& spec, const std::string& box,
                                                        const std::vector<std::string>& ids) {
  if (!spec.face) return make_error(ErrorCode::kInvalidArgument, "no font");
  if (!(spec.size > 0.0)) return make_error(ErrorCode::kInvalidArgument, "font size must be positive");
  const TextLayout layout = layout_text(*spec.face, spec.utf8, spec.size, spec.origin);
  std::vector<std::uint32_t> gids;
  for (const auto& line : layout.lines) {
    if (!visible(line)) continue;  // 공백만인 줄은 객체를 만들지 않는다
    for (const auto& g : line.glyphs) {
      if (g.gid == 0) return make_error(ErrorCode::kInvalidArgument, "font has no glyph for some characters");  // .notdef 를 넣지 않는다
      gids.push_back(g.gid);
    }
  }
  std::sort(gids.begin(), gids.end());
  gids.erase(std::unique(gids.begin(), gids.end()), gids.end());
  if (gids.empty()) return make_error(ErrorCode::kInvalidArgument, "text has no glyphs");
  auto subset = spec.face->subset(gids);
  if (!subset) return std::unexpected(subset.error());

  const std::vector<std::string> source_lines = split_lines(spec.utf8);
  engine::TextObjectsSpec out;
  out.box = box;
  // 상자 원문 전체(빈 줄 포함)를 첫 줄 객체에 보관 — 빈 줄은 객체가 없어 줄 원문만으로는 복원할 수 없다 (리뷰 W1)
  for (std::size_t i = 0; i < source_lines.size(); ++i) out.box_text += (i ? "\n" : "") + source_lines[i];
  out.font.key = subset->key;
  out.font.ttf = std::move(subset->ttf);
  out.font.cid_to_gid = std::move(subset->cid_to_gid);
  std::vector<std::pair<std::uint32_t, std::u32string>> cid_to_unicode;
  for (std::size_t i = 0; i < layout.lines.size(); ++i) {
    const auto& line = layout.lines[i];
    if (!visible(line)) continue;
    engine::TextLineSpec ls;
    ls.origin = line.baseline_origin;
    ls.font_size = spec.size;
    ls.color = spec.color;
    ls.text = i < source_lines.size() ? source_lines[i] : std::string();
    for (const auto& g : line.glyphs) {
      const std::uint32_t cid = subset->old_to_new.at(g.gid);
      ls.cids.push_back(cid);
      cid_to_unicode.emplace_back(cid, g.text);
    }
    out.lines.push_back(std::move(ls));
  }
  if (out.lines.empty()) return make_error(ErrorCode::kInvalidArgument, "text has no glyphs");
  out.font.to_unicode_cmap = make_to_unicode_cmap(std::move(cid_to_unicode));
  if (!ids.empty()) {
    if (ids.size() != out.lines.size()) return make_error(ErrorCode::kInvalidArgument, "ids/lines mismatch");
    out.ids = ids;
  }
  return out;
}

Result<TextBoxContent> collect_text_box(engine::EditableDocument& doc, int page, const std::string& box,
                                        const std::shared_ptr<const FontFace>& face) {
  if (!face) return make_error(ErrorCode::kInvalidArgument, "no font");
  auto objects = doc.text_objects(page);
  if (!objects) return std::unexpected(objects.error());
  TextBoxContent content;
  content.box = box;
  content.spec.page = page;
  content.spec.face = face;
  bool first = true;
  std::string box_text;
  for (const auto& o : *objects) {
    if (o.box != box) continue;
    content.ids.push_back(o.id);
    if (first) {
      content.spec.size = o.font_size > 0 ? o.font_size : 24.0;
      content.spec.color = o.color;
      const ShapedText metrics = face->shape("", content.spec.size);
      content.spec.origin = {o.baseline_origin.x, o.baseline_origin.y - metrics.ascent};
      content.spec.utf8 = o.text;
      box_text = o.box_text;
      first = false;
    } else {
      content.spec.utf8 += "\n" + o.text;
    }
  }
  if (content.ids.empty()) return make_error(ErrorCode::kInvalidArgument, "text box not found: " + box);
  if (!box_text.empty()) content.spec.utf8 = box_text;  // 빈 줄까지 그대로 (#47 이전 객체는 줄 원문을 이은 것)
  return content;
}

// ---------------------------------------------------------------- Remove

RemoveTextBoxCommand::RemoveTextBoxCommand(int page, std::string box, std::shared_ptr<const FontFace> face)
    : page_(page), box_(std::move(box)), face_(std::move(face)) {}

Result<engine::ChangeSet> RemoveTextBoxCommand::execute(engine::EditableDocument& doc) {
  if (!executed_) {  // 재실행은 첫 실행의 내용을 그대로 쓴다 (취소로 복원한 상자는 같은 내용)
    auto content = collect_text_box(doc, page_, box_, face_);
    if (!content) return std::unexpected(content.error());
    // 지우기 전에 되살릴 수 있는지 확인한다 — 옛 객체의 ToUnicode 원문에 U+FFFD 가 섞이면 복원이 안 된다 (리뷰 W2)
    if (auto restorable = build_text_objects_spec(content->spec, content->box, content->ids); !restorable) {
      return make_error(ErrorCode::kInvalidArgument, "cannot restore this text box after deletion: " + restorable.error().message);
    }
    content_ = std::move(*content);
  }
  if (auto removed = doc.remove_text_objects(page_, content_.ids); !removed) return std::unexpected(removed.error());
  executed_ = true;
  return page_change(page_);
}

Result<engine::ChangeSet> RemoveTextBoxCommand::undo(engine::EditableDocument& doc) {
  if (!executed_) return make_error(ErrorCode::kInvalidState, "undo before execute");
  auto spec = build_text_objects_spec(content_.spec, content_.box, content_.ids);
  if (!spec) return std::unexpected(spec.error());
  if (auto added = doc.add_text_objects(page_, *spec); !added) return std::unexpected(added.error());
  return page_change(page_);
}

// ---------------------------------------------------------------- Replace

ReplaceTextBoxCommand::ReplaceTextBoxCommand(int page, std::string box, std::shared_ptr<const FontFace> face,
                                             std::optional<std::string> utf8, std::optional<double> size,
                                             std::optional<engine::Color> color)
    : page_(page), box_(std::move(box)), face_(std::move(face)), utf8_(std::move(utf8)), size_(size), color_(color) {}

Result<engine::ChangeSet> ReplaceTextBoxCommand::execute(engine::EditableDocument& doc) {
  if (!utf8_ && !size_ && !color_) return make_error(ErrorCode::kInvalidArgument, "nothing to change");
  if (!executed_) {
    auto content = collect_text_box(doc, page_, box_, face_);
    if (!content) return std::unexpected(content.error());
    before_ = std::move(*content);
  }
  TextBoxSpec after = before_.spec;
  if (utf8_) after.utf8 = *utf8_;
  if (size_) after.size = *size_;
  if (color_) after.color = *color_;
  // 새 스펙의 줄 수를 먼저 알아야 id 를 맞출 수 있다: ids 없이 한 번 만들어 줄 수를 얻는다
  auto probe = build_text_objects_spec(after, before_.box, {});
  if (!probe) return std::unexpected(probe.error());
  if (after_ids_.empty() || after_ids_.size() != probe->lines.size()) {
    after_ids_ = reuse_ids(before_.ids, probe->lines.size());
  }
  engine::TextObjectsSpec spec = std::move(*probe);
  spec.ids = after_ids_;
  // 지우기 전에 복원 스펙을 만들어 둔다 (보상이 스펙 생성 실패로 막히지 않게, 리뷰 W2)
  auto restore = build_text_objects_spec(before_.spec, before_.box, before_.ids);
  if (!restore) return make_error(ErrorCode::kInvalidArgument, "cannot rebuild the current text box: " + restore.error().message);
  if (auto removed = doc.remove_text_objects(page_, before_.ids); !removed) return std::unexpected(removed.error());
  auto added = doc.add_text_objects(page_, spec);
  if (!added) {
    auto compensated = doc.add_text_objects(page_, *restore);  // 보상: 이전 내용으로 되살린다 (같은 id)
    if (!compensated) {
      return make_error(ErrorCode::kInvalidState, added.error().message + " (and restoring the previous text failed: " +
                                                      compensated.error().message + ")");
    }
    return std::unexpected(added.error());
  }
  after_ids_ = added->ids;
  executed_ = true;
  return page_change(page_);
}

Result<engine::ChangeSet> ReplaceTextBoxCommand::undo(engine::EditableDocument& doc) {
  if (!executed_) return make_error(ErrorCode::kInvalidState, "undo before execute");
  // 스펙을 먼저 만든다: 실패하면 아무것도 지우지 않는다 (리뷰 W2)
  auto spec = build_text_objects_spec(before_.spec, before_.box, before_.ids);
  if (!spec) return std::unexpected(spec.error());
  TextBoxSpec after = before_.spec;
  if (utf8_) after.utf8 = *utf8_;
  if (size_) after.size = *size_;
  if (color_) after.color = *color_;
  auto restore = build_text_objects_spec(after, before_.box, after_ids_);
  if (!restore) return std::unexpected(restore.error());
  if (auto removed = doc.remove_text_objects(page_, after_ids_); !removed) return std::unexpected(removed.error());
  auto added = doc.add_text_objects(page_, *spec);
  if (!added) {
    auto compensated = doc.add_text_objects(page_, *restore);
    if (!compensated) {
      return make_error(ErrorCode::kInvalidState, added.error().message + " (and restoring the edited text failed: " +
                                                      compensated.error().message + ")");
    }
    return std::unexpected(added.error());
  }
  return page_change(page_);
}

}  // namespace onpdf::edit
