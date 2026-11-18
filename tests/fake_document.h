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
  // 테스트용: 렌더를 흉내 내 AP 를 만든다 (색 변경이 AP 를 지우는지 확인)
  void fake_render(int page) {
    for (auto& a : annots_[page]) a.has_ap = true;
  }

 private:
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
};

}  // namespace onpdf::test
