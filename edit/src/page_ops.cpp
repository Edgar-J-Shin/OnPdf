#include "onpdf/edit/page_ops.h"

#include <algorithm>
#include <numeric>
#include <string>

namespace onpdf::edit {

std::vector<int> normalize_pages(std::vector<int> pages) {
  std::sort(pages.begin(), pages.end());
  pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
  return pages;
}

Result<void> validate_move(int count, const std::vector<int>& pages, int dest) {
  if (pages.empty()) return make_error(ErrorCode::kInvalidArgument, "move: no pages");
  std::vector<int> sorted = pages;
  std::sort(sorted.begin(), sorted.end());
  for (std::size_t i = 0; i < sorted.size(); ++i) {
    if (sorted[i] < 0 || sorted[i] >= count) {
      return make_error(ErrorCode::kInvalidArgument, "move: page index out of range: " + std::to_string(sorted[i]));
    }
    if (i > 0 && sorted[i] == sorted[i - 1]) return make_error(ErrorCode::kInvalidArgument, "move: duplicate page");
  }
  const int n = static_cast<int>(pages.size());
  if (dest < 0 || dest > count - n) {
    return make_error(ErrorCode::kInvalidArgument, "move: destination out of range: " + std::to_string(dest));
  }
  return {};
}

std::vector<int> simulate_move(int count, const std::vector<int>& pages, int dest) {
  std::vector<bool> moved(static_cast<std::size_t>(std::max(count, 0)), false);
  for (int p : pages) {
    if (p >= 0 && p < count) moved[static_cast<std::size_t>(p)] = true;
  }
  std::vector<int> remaining;
  remaining.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    if (!moved[static_cast<std::size_t>(i)]) remaining.push_back(i);
  }
  std::vector<int> result;
  result.reserve(static_cast<std::size_t>(count));
  const auto cut = remaining.begin() + std::clamp<std::ptrdiff_t>(dest, 0, static_cast<std::ptrdiff_t>(remaining.size()));
  result.insert(result.end(), remaining.begin(), cut);
  result.insert(result.end(), pages.begin(), pages.end());
  result.insert(result.end(), cut, remaining.end());
  return result;
}

std::vector<int> invert_order(const std::vector<int>& order) {
  std::vector<int> inverse(order.size(), 0);
  for (std::size_t new_index = 0; new_index < order.size(); ++new_index) {
    inverse[static_cast<std::size_t>(order[new_index])] = static_cast<int>(new_index);
  }
  return inverse;
}

Result<void> apply_order(engine::EditableDocument& doc, const std::vector<int>& target) {
  const int count = doc.page_count();
  if (static_cast<int>(target.size()) != count) {
    return make_error(ErrorCode::kInvalidArgument, "order size does not match page count");
  }
  // current[pos] = 그 자리에 있는 쪽의 원래(현재 기준) 인덱스
  std::vector<int> current(static_cast<std::size_t>(count));
  std::iota(current.begin(), current.end(), 0);
  for (int pos = 0; pos < count; ++pos) {
    const int wanted = target[static_cast<std::size_t>(pos)];
    const auto it = std::find(current.begin() + pos, current.end(), wanted);
    if (it == current.end()) return make_error(ErrorCode::kInvalidArgument, "order is not a permutation");
    const int from = static_cast<int>(it - current.begin());
    if (from == pos) continue;
    if (auto moved = doc.move_pages({from}, pos); !moved) return moved;
    std::rotate(current.begin() + pos, current.begin() + from, current.begin() + from + 1);
  }
  return {};
}

Result<engine::ChangeSet> structural_change(const engine::EditableDocument& doc, int focus_page,
                                            std::vector<int> selected_pages) {
  engine::ChangeSet change;
  change.structural = true;
  const int count = doc.page_count();
  for (int p : selected_pages) {
    if (p >= 0 && p < count) change.selected_pages.push_back(p);
  }
  change.focus_page = count > 0 && focus_page >= 0 ? std::min(focus_page, count - 1) : -1;
  change.all_sizes.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    auto size = doc.page_size(i);
    if (!size) return std::unexpected(size.error());
    change.all_sizes.push_back(*size);
  }
  return change;
}

}  // namespace onpdf::edit
