#include "onpdf/edit/add_text_command.h"

#include "onpdf/edit/text_layout.h"

#include <algorithm>
#include <set>

namespace onpdf::edit {
namespace {

bool is_blank(char32_t c) { return c == U' ' || c == U'\t' || c == 0xA0; }
bool visible(const LaidOutLine& line) {
  return std::any_of(line.glyphs.begin(), line.glyphs.end(), [](const ShapedGlyph& g) { return !is_blank(g.codepoint); });
}

Result<engine::ChangeSet> page_change(int page) {
  engine::ChangeSet change;
  change.pages = {page};
  return change;
}

}  // namespace

AddTextCommand::AddTextCommand(TextBoxSpec spec) : spec_(std::move(spec)) {}

Result<engine::ChangeSet> AddTextCommand::execute(engine::EditableDocument& doc) {
  if (!spec_.face) return make_error(ErrorCode::kInvalidArgument, "no font");
  if (!(spec_.size > 0.0)) return make_error(ErrorCode::kInvalidArgument, "font size must be positive");
  const TextLayout layout = layout_text(*spec_.face, spec_.utf8, spec_.size, spec_.origin);
  // 사용 글리프 집합 (빈 줄·공백만인 줄은 객체를 만들지 않는다)
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
  auto subset = spec_.face->subset(gids);
  if (!subset) return std::unexpected(subset.error());

  engine::TextObjectsSpec spec;
  spec.font.key = subset->key;
  spec.font.ttf = std::move(subset->ttf);
  spec.font.cid_to_gid = std::move(subset->cid_to_gid);
  std::vector<std::pair<std::uint32_t, char32_t>> cid_to_unicode;
  std::vector<std::size_t> line_index;  // 객체를 만든 줄의 layout 인덱스 (재실행 id 매칭)
  for (std::size_t i = 0; i < layout.lines.size(); ++i) {
    const auto& line = layout.lines[i];
    if (!visible(line)) continue;
    engine::TextLineSpec ls;
    ls.origin = line.baseline_origin;
    ls.font_size = spec_.size;
    ls.color = spec_.color;
    for (const auto& g : line.glyphs) {
      const std::uint32_t cid = subset->old_to_new.at(g.gid);
      ls.cids.push_back(cid);
      cid_to_unicode.emplace_back(cid, g.codepoint);
    }
    spec.lines.push_back(std::move(ls));
    line_index.push_back(i);
  }
  if (spec.lines.empty()) return make_error(ErrorCode::kInvalidArgument, "text has no glyphs");
  spec.font.to_unicode_cmap = make_to_unicode_cmap(std::move(cid_to_unicode));
  if (executed_ && ids_.size() == spec.lines.size()) spec.ids = ids_;  // 재실행: 같은 id
  auto ids = doc.add_text_objects(spec_.page, spec);
  if (!ids) return std::unexpected(ids.error());
  ids_ = std::move(*ids);
  executed_ = true;
  return page_change(spec_.page);
}

Result<engine::ChangeSet> AddTextCommand::undo(engine::EditableDocument& doc) {
  if (!executed_ || ids_.empty()) return make_error(ErrorCode::kInvalidState, "undo before execute");
  if (auto removed = doc.remove_text_objects(spec_.page, ids_); !removed) return std::unexpected(removed.error());
  return page_change(spec_.page);
}

}  // namespace onpdf::edit
