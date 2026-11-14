#include "onpdf/core/view/text_selection.h"

namespace onpdf::view {

TextRange TextSelection::range_on(int page, int page_char_count) const noexcept {
  if (empty()) return {};
  const TextPos s = start();
  const TextPos e = end();
  if (page < s.page || page > e.page) return {};
  const int from = page == s.page ? s.index : 0;
  const int to = page == e.page ? e.index : page_char_count;
  return {from, to};
}

std::vector<int> TextSelection::pages() const {
  std::vector<int> out;
  if (empty()) return out;
  for (int p = start().page; p <= end().page; ++p) out.push_back(p);
  return out;
}

std::string TextSelection::text(const std::function<const PageTextIndex*(int)>& provider) const {
  std::string out;
  bool first = true;
  for (int page : pages()) {
    const PageTextIndex* index = provider(page);
    if (index == nullptr) continue;
    const std::string part = index->text(range_on(page, index->char_count()));
    if (part.empty()) continue;
    if (!first) out += "\n\n";
    out += part;
    first = false;
  }
  return out;
}

}  // namespace onpdf::view
