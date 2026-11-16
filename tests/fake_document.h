#pragma once

// edit/ Command 단위 테스트용 가짜 EditableDocument. PDFium 없이 쪽 크기·회전·순서(label)·스냅샷을 흉내 낸다.
// label 은 쪽의 정체성이다: 삭제·이동·삽입 뒤 순서를 라벨 열로 확인한다

#include "onpdf/core/engine/editable_document.h"
#include "onpdf/core/platform/path.h"

#include <algorithm>
#include <map>
#include <string>
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

 private:
  [[nodiscard]] bool valid(int page) const { return page >= 0 && page < page_count(); }
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
};

}  // namespace onpdf::test
