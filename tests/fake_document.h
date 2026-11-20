#pragma once

// edit/ Command 단위 테스트용 가짜 EditableDocument. PDFium 없이 쪽 크기·회전·순서(label)·스냅샷을 흉내 낸다.
// label 은 쪽의 정체성이다: 삭제·이동·삽입 뒤 순서를 라벨 열로 확인한다

#include "onpdf/core/engine/editable_document.h"
#include "onpdf/core/platform/path.h"

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace onpdf::test {

struct FakePage {
  engine::PageSize base{612, 792};
  int rotation = 0;
  int label = 0;
};

struct FakeFile {
  std::vector<FakePage> pages;
  std::string password;
  bool extract = true;
};

class FakeDocument final : public engine::EditableDocument {
 public:
  explicit FakeDocument(std::vector<engine::PageSize> sizes) {
    for (std::size_t i = 0; i < sizes.size(); ++i) pages_.push_back({sizes[i], 0, static_cast<int>(i)});
  }

  // ---- 테스트 보조 ----
  [[nodiscard]] std::vector<int> labels() const {
    std::vector<int> out;
    for (const auto& p : pages_) out.push_back(p.label);
    return out;
  }
  [[nodiscard]] std::vector<int> rotations() const {
    std::vector<int> out;
    for (const auto& p : pages_) out.push_back(p.rotation);
    return out;
  }
  void add_file(const std::filesystem::path& path, FakeFile file) { files_[path_to_utf8(path)] = std::move(file); }
  [[nodiscard]] int snapshot_count() const noexcept { return static_cast<int>(snapshots_.size()); }
  int move_calls = 0;  // move_pages 호출 수 (apply_order 효율 확인용)
  // ---- 주석 (가짜: 쪽 포인트 == 사용자 좌표로 둔다) ----
  struct FakeAnnot {
    engine::AnnotationSnapshot snap;
    onpdf::Rect rect;  // 쪽 포인트
    bool has_ap = false;  // 렌더로 생긴 AP (색 변경·스냅샷이 지운다)
  };
  int fail_add_on_page = -1;  // 이 쪽에 add_annotation 을 하면 실패 (보상 삭제 테스트)
  int fail_restores = 0;      // 다음 n 번의 restore_annotation 이 실패 (재생성 보상 테스트)
  // ---- 텍스트 객체 (가짜: 줄 스펙과 폰트 key 를 그대로 보관) ----
  struct FakeTextObject {
    std::string id;
    std::string box;
    std::string box_text;  // 첫 줄에만
    double width = 0.0;    // 첫 줄에만
    engine::TextLineSpec line;
    std::string font_key;
  };
  int fail_add_text_on_page = -1;
  int fail_next_add_text = 0;  // 다음 n 번의 add_text_objects 가 실패 (수정 보상 테스트)
  [[nodiscard]] const std::vector<FakeTextObject>& text_objects_on(int page) const {
    static const std::vector<FakeTextObject> kEmpty;
    const auto it = texts_.find(page);
    return it == texts_.end() ? kEmpty : it->second;
  }
  [[nodiscard]] int font_count() const noexcept { return static_cast<int>(font_keys_.size()); }
  [[nodiscard]] Result<engine::TextObjectsResult> add_text_objects(int page, const engine::TextObjectsSpec& spec) override {
    if (!valid(page)) return make_error(ErrorCode::kPageNotFound, "page");
    if (page == fail_add_text_on_page) return make_error(ErrorCode::kInvalidState, "injected failure");
    if (fail_next_add_text > 0) { --fail_next_add_text; return make_error(ErrorCode::kInvalidState, "injected add failure"); }
    if (spec.lines.empty() || spec.font.key.empty()) return make_error(ErrorCode::kInvalidArgument, "spec");
    if (!spec.ids.empty() && spec.ids.size() != spec.lines.size()) return make_error(ErrorCode::kInvalidArgument, "ids");
    font_keys_[spec.font.key] = spec.font.ttf.size();
    engine::TextObjectsResult result;
    result.box = spec.box.empty() ? "fake-box-" + std::to_string(next_text_++) : spec.box;
    for (std::size_t i = 0; i < spec.lines.size(); ++i) {
      FakeTextObject t;
      t.id = spec.ids.empty() ? "fake-text-" + std::to_string(next_text_++) : spec.ids[i];
      t.box = spec.write_box ? result.box : t.id;
      t.line = spec.lines[i];
      if (spec.write_box && i == 0) t.box_text = spec.box_text;
      if (spec.write_box && i == 0) t.width = spec.width;
      if (!spec.write_box) t.line.text.clear();
      t.font_key = spec.font.key;
      texts_[page].push_back(std::move(t));
      result.ids.push_back(texts_[page].back().id);
    }
    return result;
  }
  [[nodiscard]] Result<void> move_page_objects(int page, const std::vector<std::string>& ids, onpdf::Point delta) override {
    auto& list = texts_[page];
    auto& images = images_[page];
    for (const auto& id : ids) {
      const bool text = std::any_of(list.begin(), list.end(), [&](const FakeTextObject& t) { return t.id == id; });
      const bool image = std::any_of(images.begin(), images.end(), [&](const FakeImage& i) { return i.id == id; });
      if (!text && !image) return make_error(ErrorCode::kInvalidArgument, "page object not found: " + id);
    }
    for (auto& t : list) {
      if (std::find(ids.begin(), ids.end(), t.id) != ids.end()) { t.line.origin.x += delta.x; t.line.origin.y += delta.y; }
    }
    for (auto& i : images) {
      if (std::find(ids.begin(), ids.end(), i.id) != ids.end()) { i.rect.x += delta.x; i.rect.y += delta.y; }
    }
    return {};
  }
  // ---- 이미지·쪽 객체 일반 (#49, 가짜: 이미지 목록 + 보관소) ----
  struct FakeImage {
    std::string id;
    bool marked = true;
    onpdf::Rect rect;
    int pixel_width = 0, pixel_height = 0;
    std::string filter;
  };
  int fail_add_image = 0;
  [[nodiscard]] const std::vector<FakeImage>& images_on(int page) const {
    static const std::vector<FakeImage> kEmpty;
    const auto it = images_.find(page);
    return it == images_.end() ? kEmpty : it->second;
  }
  [[nodiscard]] int detached_count() const noexcept { return static_cast<int>(detached_.size()); }
  // 테스트용: 다른 프로그램이 넣은 (마크 없는) 이미지
  void add_foreign_image(int page, onpdf::Rect rect) {
    FakeImage image;
    image.marked = false;
    image.rect = rect;
    image.pixel_width = 10;
    image.pixel_height = 10;
    image.filter = "FlateDecode";
    images_[page].push_back(image);
    renumber_foreign(page);
  }
  [[nodiscard]] Result<std::string> add_image_object(int page, const engine::ImageSpec& spec) override {
    if (!valid(page)) return make_error(ErrorCode::kPageNotFound, "page");
    if (fail_add_image > 0) { --fail_add_image; return make_error(ErrorCode::kInvalidState, "injected image failure"); }
    if (!(spec.rect.w > 0) || !(spec.rect.h > 0)) return make_error(ErrorCode::kInvalidArgument, "rect");
    FakeImage image;
    image.id = spec.id.empty() ? "fake-image-" + std::to_string(next_text_++) : spec.id;
    image.rect = spec.rect;
    if (const auto* p = std::get_if<engine::ImagePixels>(&spec.source)) {
      image.pixel_width = p->width;
      image.pixel_height = p->height;
      image.filter = "FlateDecode";
    } else {
      const auto& j = std::get<engine::JpegBytes>(spec.source);
      image.pixel_width = j.width;
      image.pixel_height = j.height;
      image.filter = "DCTDecode";
    }
    images_[page].push_back(image);
    return image.id;
  }
  [[nodiscard]] Result<std::vector<engine::PageObjectInfo>> page_objects(int page) const override {
    if (!valid(page)) return make_error(ErrorCode::kPageNotFound, "page");
    std::vector<engine::PageObjectInfo> out;
    auto texts = text_objects(page);
    int index = 0;
    for (auto& t : *texts) {
      engine::PageObjectInfo info;
      info.kind = engine::ObjectKind::kText;
      info.id = t.id;
      info.marked = true;
      info.index = index++;
      info.bbox = t.bbox;
      info.text = t;
      out.push_back(std::move(info));
    }
    for (const auto& i : images_on(page)) {
      engine::PageObjectInfo info;
      info.kind = engine::ObjectKind::kImage;
      info.id = i.id;
      info.marked = i.marked;
      info.index = index++;
      info.bbox = i.rect;
      info.pixel_width = i.pixel_width;
      info.pixel_height = i.pixel_height;
      info.filter = i.filter;
      out.push_back(std::move(info));
    }
    return out;
  }
  [[nodiscard]] Result<std::string> mark_object(int page, std::string_view id) override {
    for (auto& i : images_[page]) {
      if (i.id != id) continue;
      if (!i.marked) {
        i.marked = true;
        i.id = "fake-image-" + std::to_string(next_text_++);
        renumber_foreign(page);
      }
      return i.id;
    }
    return make_error(ErrorCode::kInvalidArgument, "image not found");
  }
  [[nodiscard]] Result<void> set_object_rect(int page, std::string_view id, onpdf::Rect rect) override {
    if (!(rect.w > 0) || !(rect.h > 0)) return make_error(ErrorCode::kInvalidArgument, "rect");
    for (auto& i : images_[page]) {
      if (i.id == id) { i.rect = rect; return {}; }
    }
    return make_error(ErrorCode::kInvalidArgument, "image not found");
  }
  [[nodiscard]] Result<engine::DetachedId> detach_objects(int page, const std::vector<std::string>& ids) override {
    FakeDetached d;
    d.page = page;
    auto& texts = texts_[page];
    auto& images = images_[page];
    for (const auto& id : ids) {
      const bool found = std::any_of(texts.begin(), texts.end(), [&](const FakeTextObject& t) { return t.id == id; }) ||
                         std::any_of(images.begin(), images.end(), [&](const FakeImage& i) { return i.id == id; });
      if (!found) return make_error(ErrorCode::kInvalidArgument, "page object not found: " + id);
    }
    for (std::size_t k = 0; k < texts.size();) {
      if (std::find(ids.begin(), ids.end(), texts[k].id) != ids.end()) {
        d.texts.emplace_back(k + d.texts.size(), texts[k]);
        texts.erase(texts.begin() + static_cast<std::ptrdiff_t>(k));
      } else {
        ++k;
      }
    }
    for (std::size_t k = 0; k < images.size();) {
      if (std::find(ids.begin(), ids.end(), images[k].id) != ids.end()) {
        d.images.emplace_back(k + d.images.size(), images[k]);
        images.erase(images.begin() + static_cast<std::ptrdiff_t>(k));
      } else {
        ++k;
      }
    }
    const engine::DetachedId token = next_detached_++;
    detached_[token] = std::move(d);
    return token;
  }
  [[nodiscard]] Result<void> reattach_objects(engine::DetachedId id) override {
    auto it = detached_.find(id);
    if (it == detached_.end()) return make_error(ErrorCode::kInvalidArgument, "unknown detached");
    auto& texts = texts_[it->second.page];
    auto& images = images_[it->second.page];
    for (auto& [index, t] : it->second.texts) texts.insert(texts.begin() + static_cast<std::ptrdiff_t>(std::min(index, texts.size())), t);
    for (auto& [index, i] : it->second.images) images.insert(images.begin() + static_cast<std::ptrdiff_t>(std::min(index, images.size())), i);
    detached_.erase(it);
    return {};
  }
  void drop_detached(engine::DetachedId id) noexcept override { detached_.erase(id); }
  [[nodiscard]] Result<void> remove_text_objects(int page, const std::vector<std::string>& ids) override {
    auto& list = texts_[page];
    for (const auto& id : ids) {
      if (std::none_of(list.begin(), list.end(), [&](const FakeTextObject& t) { return t.id == id; })) {
        return make_error(ErrorCode::kInvalidArgument, "text object not found: " + id);
      }
    }
    for (const auto& id : ids) {
      list.erase(std::remove_if(list.begin(), list.end(), [&](const FakeTextObject& t) { return t.id == id; }), list.end());
    }
    return {};
  }
  [[nodiscard]] Result<std::vector<engine::TextObjectInfo>> text_objects(int page) const override {
    if (!valid(page)) return make_error(ErrorCode::kPageNotFound, "page");
    std::vector<engine::TextObjectInfo> out;
    for (const auto& t : text_objects_on(page)) {
      engine::TextObjectInfo info;
      info.id = t.id;
      info.box = t.box;
      info.text = t.line.text;
      info.box_text = t.box_text;
      info.width = t.width;
      info.font_size = t.line.font_size;
      info.color = t.line.color;
      info.baseline_origin = t.line.origin;
      info.bbox = {t.line.origin.x, t.line.origin.y - t.line.font_size, 100.0, t.line.font_size * 1.2};
      out.push_back(std::move(info));
    }
    return out;
  }
  // ---- 기존 텍스트 줄 (#65): 가짜 줄 목록 + 상태 토큰(전체 가짜 상태 복사) ----
  struct FakeLine {
    std::string id;
    std::string text;
    bool editable = true;
    bool covers = true;  // 원본 글꼴이 새 글자를 그릴 수 있다
    double size = 12.0;
  };
  void add_line(int page, std::string id, std::string text, bool covers = true, bool editable = true) {
    lines_[page].push_back(FakeLine{std::move(id), std::move(text), editable, covers, 12.0});
  }
  [[nodiscard]] const std::vector<FakeLine>& lines_on(int page) const {
    static const std::vector<FakeLine> kEmpty;
    const auto it = lines_.find(page);
    return it == lines_.end() ? kEmpty : it->second;
  }
  [[nodiscard]] int document_state_count() const noexcept { return static_cast<int>(states_.size()); }
  bool fail_next_line_edit = false;
  [[nodiscard]] Result<std::vector<engine::TextLineInfo>> text_lines(int page) const override {
    if (!valid(page)) return make_error(ErrorCode::kPageNotFound, "page");
    std::vector<engine::TextLineInfo> out;
    double y = 100.0;
    for (const auto& l : lines_on(page)) {
      engine::TextLineInfo info;
      info.id = l.id;
      info.text = l.text;
      info.bbox = {72.0, y, 200.0, l.size * 1.2};
      info.font_size = l.size;
      info.font_name = "Fake";
      info.font_embedded = true;
      info.editable = l.editable;
      if (!l.editable) info.reason = "fake";
      out.push_back(std::move(info));
      y += l.size * 1.5;
    }
    return out;
  }
  [[nodiscard]] Result<bool> line_font_covers(int page, std::string_view line_id, std::string_view) const override {
    if (line_id.starts_with("onpdf-box-") || line_id.rfind("fake-box-", 0) == 0) {
      for (const auto& t : text_objects_on(page)) if (t.box == line_id) return t.font_key == "original";
      return make_error(ErrorCode::kInvalidArgument, "line not found");
    }
    for (const auto& l : lines_on(page)) if (l.id == line_id) return l.covers;
    return make_error(ErrorCode::kInvalidArgument, "line not found");
  }
  [[nodiscard]] Result<engine::LineEditResult> remove_text_line(int page, std::string_view line_id) override {
    return edit_fake_line(page, line_id, nullptr);
  }
  [[nodiscard]] Result<engine::LineEditResult> replace_text_line(int page, std::string_view line_id,
                                                                 const engine::LineReplacement& r) override {
    return edit_fake_line(page, line_id, &r);
  }
  [[nodiscard]] Result<void> restore_document_state(engine::DocumentStateId id) override {
    auto it = states_.find(id);
    if (it == states_.end()) return make_error(ErrorCode::kInvalidState, "undo history evicted");
    texts_ = it->second.texts;
    lines_ = it->second.lines;
    images_ = it->second.images;
    return {};
  }
  void drop_document_state(engine::DocumentStateId id) noexcept override { states_.erase(id); }

  // ---- 폼 (#51): 쪽별 위젯. 같은 이름의 위젯은 값을 공유한다 ----
  void add_widget(int page, engine::FormWidgetInfo info) { widgets_[page].push_back(std::move(info)); }
  bool fail_form = false;
  [[nodiscard]] const engine::FormWidgetInfo* widget(int page, const engine::FieldRef& ref) const {
    const auto it = widgets_.find(page);
    if (it == widgets_.end()) return nullptr;
    for (const auto& w : it->second) if (w.ref == ref) return &w;
    return nullptr;
  }
  [[nodiscard]] Result<std::vector<engine::FormWidgetInfo>> form_widgets(int page) const override {
    if (!valid(page)) return make_error(ErrorCode::kPageNotFound, "page");
    const auto it = widgets_.find(page);
    return it == widgets_.end() ? std::vector<engine::FormWidgetInfo>{} : it->second;
  }
  [[nodiscard]] Result<std::vector<int>> set_form_text(int page, const engine::FieldRef& ref, std::string_view utf8) override {
    auto target = mutable_widget(page, ref);
    if (!target) return std::unexpected(target.error());
    if ((*target)->type != engine::FieldType::kText && !(*target)->editable_combo) return make_error(ErrorCode::kInvalidArgument, "not text");
    std::string value(utf8);
    if ((*target)->max_len > 0 && value.size() > static_cast<std::size_t>((*target)->max_len)) value.resize(static_cast<std::size_t>((*target)->max_len));
    return for_field(ref.name, [&](engine::FormWidgetInfo& w) {
      w.value = value;
      w.selected.clear();
      for (std::size_t i = 0; i < w.options.size(); ++i) if (w.options[i] == value) w.selected.push_back(static_cast<int>(i));
    });
  }
  [[nodiscard]] Result<std::vector<int>> set_form_checked(int page, const engine::FieldRef& ref, bool checked) override {
    auto target = mutable_widget(page, ref);
    if (!target) return std::unexpected(target.error());
    const engine::FieldType type = (*target)->type;
    if (type == engine::FieldType::kRadio && !checked && (*target)->checked) return make_error(ErrorCode::kUnsupported, "radio clear");
    const std::string on = (*target)->export_value;
    return for_field(ref.name, [&](engine::FormWidgetInfo& w) {
      if (type == engine::FieldType::kRadio) {
        w.checked = checked && w.ref == ref ? true : (checked ? false : w.checked);
      } else {
        w.checked = checked;
      }
      w.value = checked ? on : (type == engine::FieldType::kRadio ? w.value : "Off");
    });
  }
  [[nodiscard]] Result<std::vector<int>> set_form_selection(int page, const engine::FieldRef& ref, const std::vector<int>& indices) override {
    auto target = mutable_widget(page, ref);
    if (!target) return std::unexpected(target.error());
    for (int i : indices) if (i < 0 || i >= static_cast<int>((*target)->options.size())) return make_error(ErrorCode::kInvalidArgument, "index");
    return for_field(ref.name, [&](engine::FormWidgetInfo& w) {
      w.selected = indices;
      w.value = indices.empty() ? std::string() : w.options[static_cast<std::size_t>(indices.front())];
    });
  }

  [[nodiscard]] int annotation_count(int page) const { const auto it = annots_.find(page); return it == annots_.end() ? 0 : static_cast<int>(it->second.size()); }
  [[nodiscard]] const FakeAnnot* annotation(int page, std::string_view id) const {
    const auto it = annots_.find(page);
    if (it == annots_.end()) return nullptr;
    for (const auto& a : it->second) if (a.snap.id == id) return &a;
    return nullptr;
  }

  // ---- EditableDocument ----
  [[nodiscard]] int page_count() const noexcept override { return static_cast<int>(pages_.size()); }
  [[nodiscard]] Result<engine::PageSize> page_size(int page) const override {
    if (!valid(page)) return make_error(ErrorCode::kPageNotFound, "page");
    const FakePage& p = pages_[static_cast<std::size_t>(page)];
    return p.rotation % 2 ? engine::PageSize{p.base.height, p.base.width} : p.base;
  }
  [[nodiscard]] Result<int> page_rotation(int page) const override {
    if (!valid(page)) return make_error(ErrorCode::kPageNotFound, "page");
    return pages_[static_cast<std::size_t>(page)].rotation;
  }
  [[nodiscard]] Result<void> set_page_rotation(int page, int q) override {
    if (!valid(page)) return make_error(ErrorCode::kPageNotFound, "page");
    pages_[static_cast<std::size_t>(page)].rotation = q;
    return {};
  }
  [[nodiscard]] Result<void> delete_pages(const std::vector<int>& pages) override {
    if (auto ok = check_list(pages, page_count()); !ok) return ok;
    if (static_cast<int>(pages.size()) >= page_count()) return make_error(ErrorCode::kInvalidArgument, "all");
    for (auto it = pages.rbegin(); it != pages.rend(); ++it) pages_.erase(pages_.begin() + *it);
    return {};
  }
  [[nodiscard]] Result<void> move_pages(const std::vector<int>& pages, int dest) override {
    const int count = page_count();
    std::vector<int> sorted = pages;
    std::sort(sorted.begin(), sorted.end());
    if (auto ok = check_list(sorted, count); !ok) return ok;
    if (dest < 0 || dest > count - static_cast<int>(pages.size())) return make_error(ErrorCode::kInvalidArgument, "dest");
    ++move_calls;
    std::vector<FakePage> block;
    for (int p : pages) block.push_back(pages_[static_cast<std::size_t>(p)]);
    std::vector<FakePage> remaining;
    for (int i = 0; i < count; ++i) {
      if (std::find(pages.begin(), pages.end(), i) == pages.end()) remaining.push_back(pages_[static_cast<std::size_t>(i)]);
    }
    remaining.insert(remaining.begin() + dest, block.begin(), block.end());
    pages_ = std::move(remaining);
    return {};
  }
  [[nodiscard]] Result<engine::SnapshotId> copy_pages_to_snapshot(const std::vector<int>& pages) override {
    if (auto ok = check_list(pages, page_count()); !ok) return std::unexpected(ok.error());
    FakeFile snap;
    for (int p : pages) snap.pages.push_back(pages_[static_cast<std::size_t>(p)]);
    snapshots_[next_] = std::move(snap);
    return next_++;
  }
  [[nodiscard]] Result<engine::SnapshotId> open_snapshot(const std::filesystem::path& file,
                                                         std::string_view password) override {
    const auto it = files_.find(path_to_utf8(file));
    if (it == files_.end()) return make_error(ErrorCode::kFileNotFound, "no such fake file");
    if (it->second.password != password) return make_error(ErrorCode::kPasswordRequired, "password");
    snapshots_[next_] = it->second;
    return next_++;
  }
  [[nodiscard]] Result<engine::SnapshotInfo> snapshot_info(engine::SnapshotId id) const override {
    const auto it = snapshots_.find(id);
    if (it == snapshots_.end()) return make_error(ErrorCode::kInvalidArgument, "snapshot");
    engine::SnapshotInfo info;
    info.page_count = static_cast<int>(it->second.pages.size());
    info.permissions.extract = it->second.extract;
    return info;
  }
  [[nodiscard]] Result<void> insert_from_snapshot(engine::SnapshotId id, const std::vector<int>& src_pages,
                                                  int at) override {
    const auto it = snapshots_.find(id);
    if (it == snapshots_.end()) return make_error(ErrorCode::kInvalidArgument, "snapshot");
    if (at < 0 || at > page_count()) return make_error(ErrorCode::kInvalidArgument, "at");
    std::vector<FakePage> block;
    if (src_pages.empty()) {
      block = it->second.pages;
    } else {
      for (int p : src_pages) {
        if (p < 0 || p >= static_cast<int>(it->second.pages.size())) return make_error(ErrorCode::kInvalidArgument, "src");
        block.push_back(it->second.pages[static_cast<std::size_t>(p)]);
      }
    }
    pages_.insert(pages_.begin() + at, block.begin(), block.end());
    return {};
  }
  void drop_snapshot(engine::SnapshotId id) noexcept override { snapshots_.erase(id); }

  [[nodiscard]] Result<std::vector<engine::AnnotationInfo>> annotations(int page) const override {
    if (!valid(page)) return make_error(ErrorCode::kPageNotFound, "page");
    std::vector<engine::AnnotationInfo> out;
    const auto it = annots_.find(page);
    if (it == annots_.end()) return out;
    for (std::size_t i = 0; i < it->second.size(); ++i) {
      const FakeAnnot& a = it->second[i];
      engine::AnnotationInfo info;
      info.id = a.snap.id.empty() ? "#" + std::to_string(i) : a.snap.id;
      info.kind = a.snap.kind;
      info.rect = a.rect;
      info.color = a.snap.color;
      info.has_color = !a.has_ap;
      info.contents = a.snap.contents;
      info.author = a.snap.author;
      info.flags = a.snap.flags;
      info.editable = a.snap.kind != engine::AnnotationKind::kLink && a.snap.kind != engine::AnnotationKind::kOther;
      info.border_width = a.snap.border_width;
      info.has_border = a.snap.has_border;
      info.opacity = a.snap.color.a / 255.0f;
      info.has_bs = a.snap.has_bs;
      out.push_back(std::move(info));
    }
    return out;
  }
  [[nodiscard]] Result<std::string> add_annotation(int page, const engine::AnnotationSpec& spec) override {
    if (!valid(page)) return make_error(ErrorCode::kPageNotFound, "page");
    if (page == fail_add_on_page) return make_error(ErrorCode::kInvalidState, "injected failure");
    FakeAnnot a;
    a.snap.kind = spec.kind;
    a.snap.id = spec.id.empty() ? "fake-" + std::to_string(next_nm_++) : spec.id;
    a.snap.color = spec.color;
    a.snap.has_color = true;
    a.snap.flags = spec.flags;
    a.snap.contents = spec.contents;
    a.snap.author = spec.author;
    a.snap.has_border = spec.border_width > 0;
    a.snap.border_width = static_cast<float>(spec.border_width);
    if (spec.interior) { a.snap.interior = *spec.interior; a.snap.has_interior = true; }
    a.rect = spec.rect;
    for (const auto& q : spec.quads) {
      if (a.rect.empty()) { a.rect = q; continue; }
      const double x0 = std::min(a.rect.x, q.x), y0 = std::min(a.rect.y, q.y);
      const double x1 = std::max(a.rect.right(), q.right()), y1 = std::max(a.rect.bottom(), q.bottom());
      a.rect = {x0, y0, x1 - x0, y1 - y0};
    }
    a.snap.rect = {a.rect.x, a.rect.y, a.rect.right(), a.rect.bottom()};
    annots_[page].push_back(std::move(a));
    return annots_[page].back().snap.id;
  }
  [[nodiscard]] Result<engine::AnnotationSnapshot> snapshot_annotation(int page, std::string_view id) override {
    FakeAnnot* a = find_mutable(page, id);
    if (a == nullptr) return make_error(ErrorCode::kInvalidArgument, "annotation not found");
    if (a->snap.id.empty()) a->snap.id = "fake-" + std::to_string(next_nm_++);
    a->has_ap = false;  // 실제 엔진처럼 AP 를 지우고 읽는다
    return a->snap;
  }
  [[nodiscard]] Result<void> remove_annotation(int page, std::string_view id) override {
    auto& list = annots_[page];
    for (auto it = list.begin(); it != list.end(); ++it) {
      if (matches(*it, static_cast<int>(it - list.begin()), id)) {
        list.erase(it);
        return {};
      }
    }
    return make_error(ErrorCode::kInvalidArgument, "annotation not found");
  }
  [[nodiscard]] Result<void> restore_annotation(int page, const engine::AnnotationSnapshot& snap) override {
    if (!valid(page)) return make_error(ErrorCode::kPageNotFound, "page");
    if (fail_restores > 0) { --fail_restores; return make_error(ErrorCode::kInvalidState, "injected restore failure"); }
    FakeAnnot a;
    a.snap = snap;
    a.rect = {snap.rect[0], snap.rect[1], snap.rect[2] - snap.rect[0], snap.rect[3] - snap.rect[1]};
    annots_[page].push_back(std::move(a));
    return {};
  }
  [[nodiscard]] Result<void> set_annotation_color(int page, std::string_view id, engine::Color color) override {
    FakeAnnot* a = find_mutable(page, id);
    if (a == nullptr) return make_error(ErrorCode::kInvalidArgument, "annotation not found");
    a->has_ap = false;
    a->snap.color = color;
    return {};
  }
  [[nodiscard]] Result<void> set_annotation_contents(int page, std::string_view id, std::string_view utf8) override {
    FakeAnnot* a = find_mutable(page, id);
    if (a == nullptr) return make_error(ErrorCode::kInvalidArgument, "annotation not found");
    a->snap.contents = std::string(utf8);
    return {};
  }
  [[nodiscard]] Result<void> set_annotation_border_width(int page, std::string_view id, double width) override {
    FakeAnnot* a = find_mutable(page, id);
    if (a == nullptr) return make_error(ErrorCode::kInvalidArgument, "annotation not found");
    if (a->snap.has_bs) return make_error(ErrorCode::kInvalidArgument, "border width is fixed by /BS");
    if (!(width > 0)) return make_error(ErrorCode::kInvalidArgument, "width");
    a->has_ap = false;
    a->snap.border_width = static_cast<float>(width);
    a->snap.has_border = true;
    return {};
  }
  // 테스트용: 다른 프로그램이 /BS /W 로 만든 주석을 흉내 낸다 (/Border 는 없음)
  void set_bs(int page, std::string_view id) {
    if (FakeAnnot* a = find_mutable(page, id)) { a->snap.has_bs = true; a->snap.has_border = false; a->snap.border_width = 0; }
  }
  // 테스트용: 렌더를 흉내 내 AP 를 만든다 (색 변경이 AP 를 지우는지 확인)
  void fake_render(int page) {
    for (auto& a : annots_[page]) a.has_ap = true;
  }

 private:
  Result<engine::FormWidgetInfo*> mutable_widget(int page, const engine::FieldRef& ref) {
    if (fail_form) return make_error(ErrorCode::kInvalidState, "form failure");
    if (!valid(page)) return make_error(ErrorCode::kPageNotFound, "page");
    for (auto& w : widgets_[page]) {
      if (w.ref != ref) continue;
      if (w.read_only) return make_error(ErrorCode::kPermissionDenied, "read-only");
      return &w;
    }
    return make_error(ErrorCode::kInvalidArgument, "field not found: " + ref.name);
  }
  template <class F>
  std::vector<int> for_field(const std::string& name, F&& apply) {
    std::vector<int> pages;
    for (auto& [p, list] : widgets_) {
      for (auto& w : list) {
        if (w.ref.name != name) continue;
        apply(w);
        if (pages.empty() || pages.back() != p) pages.push_back(p);
      }
    }
    return pages;
  }
  [[nodiscard]] bool valid(int page) const { return page >= 0 && page < page_count(); }
  static bool matches(const FakeAnnot& a, int index, std::string_view id) {
    if (!id.empty() && id.front() == '#') return std::to_string(index) == id.substr(1);
    return a.snap.id == id;
  }
  FakeAnnot* find_mutable(int page, std::string_view id) {
    auto it = annots_.find(page);
    if (it == annots_.end()) return nullptr;
    for (std::size_t i = 0; i < it->second.size(); ++i) if (matches(it->second[i], static_cast<int>(i), id)) return &it->second[i];
    return nullptr;
  }
  static Result<void> check_list(const std::vector<int>& pages, int count) {
    if (pages.empty()) return make_error(ErrorCode::kInvalidArgument, "empty");
    for (std::size_t i = 0; i < pages.size(); ++i) {
      if (pages[i] < 0 || pages[i] >= count) return make_error(ErrorCode::kInvalidArgument, "range");
      if (i > 0 && pages[i] <= pages[i - 1]) return make_error(ErrorCode::kInvalidArgument, "order");
    }
    return {};
  }
  std::vector<FakePage> pages_;
  std::map<engine::SnapshotId, FakeFile> snapshots_;
  std::map<std::string, FakeFile> files_;
  engine::SnapshotId next_ = 1;
  std::map<int, std::vector<FakeAnnot>> annots_;
  int next_nm_ = 1;
  std::map<int, std::vector<FakeTextObject>> texts_;
  std::map<int, std::vector<FakeImage>> images_;
  std::map<int, std::vector<engine::FormWidgetInfo>> widgets_;
  struct FakeDetached {
    int page = 0;
    std::vector<std::pair<std::size_t, FakeTextObject>> texts;
    std::vector<std::pair<std::size_t, FakeImage>> images;
  };
  std::map<engine::DetachedId, FakeDetached> detached_;
  engine::DetachedId next_detached_ = 1;
  // 마크 없는 이미지의 id 는 "#obj<index>" (엔진과 같은 규칙)
  void renumber_foreign(int page) {
    auto& images = images_[page];
    const int base = static_cast<int>(texts_[page].size());
    for (std::size_t k = 0; k < images.size(); ++k) {
      if (!images[k].marked) images[k].id = "#obj" + std::to_string(base + static_cast<int>(k));
    }
  }
  std::map<int, std::vector<FakeLine>> lines_;
  struct FakeState {
    std::map<int, std::vector<FakeTextObject>> texts;
    std::map<int, std::vector<FakeLine>> lines;
    std::map<int, std::vector<FakeImage>> images;
  };
  std::map<engine::DocumentStateId, FakeState> states_;
  engine::DocumentStateId next_state_ = 1;
  Result<engine::LineEditResult> edit_fake_line(int page, std::string_view line_id, const engine::LineReplacement* r) {
    if (!valid(page)) return make_error(ErrorCode::kPageNotFound, "page");
    if (fail_next_line_edit) {
      fail_next_line_edit = false;
      return make_error(ErrorCode::kJobFailed, "injected");
    }
    engine::LineEditResult result;
    const bool onpdf_box = line_id.starts_with("onpdf-box-") || line_id.rfind("fake-box-", 0) == 0;
    auto covers = line_font_covers(page, line_id, r ? r->utf8 : "");
    if (!covers) return std::unexpected(covers.error());
    if (!onpdf_box) {
      auto& lines = lines_[page];
      auto it = std::find_if(lines.begin(), lines.end(), [&](const FakeLine& l) { return l.id == line_id; });
      if (it == lines.end()) return make_error(ErrorCode::kInvalidArgument, "line not found");
      if (!it->editable) return make_error(ErrorCode::kUnsupported, "fake");
      if (r != nullptr && !*covers && !r->fallback) return make_error(ErrorCode::kInvalidArgument, "original font cannot draw the text");
      result.before = next_state_++;
      states_[result.before] = FakeState{texts_, lines_, images_};
      lines.erase(it);
    } else {
      auto& texts = texts_[page];
      const bool any = std::any_of(texts.begin(), texts.end(), [&](const FakeTextObject& t) { return t.box == line_id; });
      if (!any) return make_error(ErrorCode::kInvalidArgument, "text box not found");
      texts.erase(std::remove_if(texts.begin(), texts.end(), [&](const FakeTextObject& t) { return t.box == line_id; }), texts.end());
    }
    if (r != nullptr) {
      FakeTextObject t;
      t.id = "fake-text-" + std::to_string(next_text_++);
      t.box = onpdf_box ? std::string(line_id) : "fake-box-" + std::to_string(next_text_++);
      t.line.text = r->utf8;
      t.line.font_size = 12.0;
      t.box_text = r->utf8;
      if (*covers) {
        t.font_key = "original";
      } else {
        t.font_key = r->fallback ? r->fallback->font.key : "";
        font_keys_[t.font_key] = r->fallback ? r->fallback->font.ttf.size() : 0;
        result.used_fallback = true;
      }
      result.box = t.box;
      result.ids = {t.id};
      texts_[page].push_back(std::move(t));
    }
    return result;
  }
  std::map<std::string, std::size_t> font_keys_;  // key → ttf 크기
  int next_text_ = 1;
};

}  // namespace onpdf::test
