#include "onpdf/core/view/outline.h"

#include <algorithm>

namespace onpdf::view {

void Outline::build_position_index() {
  by_position_.clear();
  for (int i = 0; i < size(); ++i) {
    const auto& item = items[static_cast<std::size_t>(i)];
    if (!item.target || item.target->kind != LinkKind::kPage || item.target->page < 0) continue;
    by_position_.push_back({item.target->page, item.approx_y, i});
  }
  std::sort(by_position_.begin(), by_position_.end(), [](const Key& a, const Key& b) {
    if (a.page != b.page) return a.page < b.page;
    if (a.y != b.y) return a.y < b.y;
    return a.index < b.index;
  });
}

int Outline::item_at(int page, double y_pt) const {
  // (page, y) 이하인 마지막 키
  auto it = std::upper_bound(by_position_.begin(), by_position_.end(), std::make_pair(page, y_pt),
                             [](const std::pair<int, double>& v, const Key& k) {
                               if (v.first != k.page) return v.first < k.page;
                               return v.second < k.y;
                             });
  if (it == by_position_.begin()) return -1;
  return std::prev(it)->index;
}

}  // namespace onpdf::view
