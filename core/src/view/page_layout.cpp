#include "onpdf/core/view/page_layout.h"

#include "onpdf/core/view/tile.h"

#include <algorithm>
#include <cmath>

namespace onpdf::view {

PageLayout::PageLayout(std::vector<engine::PageSize> pages_pt, LayoutOptions options)
    : pages_pt_(std::move(pages_pt)), options_(options) {
  for (const auto& p : pages_pt_) max_page_width_pt_ = std::max(max_page_width_pt_, p.width);
  rebuild();
}

void PageLayout::set_scale_q(int scale_q) {
  scale_q_ = std::max(kMinScaleQ, scale_q);
  rebuild();
}

double PageLayout::scale() const noexcept { return dequantize_scale(scale_q_); }

void PageLayout::rebuild() {
  tops_.assign(pages_pt_.size() + 1, 0.0);
  max_page_width_px_ = 0.0;
  double y = options_.margin;
  for (std::size_t i = 0; i < pages_pt_.size(); ++i) {
    tops_[i] = y;
    const Size px = page_pixel_size(pages_pt_[i], scale_q_);
    max_page_width_px_ = std::max(max_page_width_px_, px.w);
    y += px.h + options_.gap;
  }
  tops_[pages_pt_.size()] = y;
}

Rect PageLayout::page_rect(int page) const {
  const auto i = static_cast<std::size_t>(page);
  const Size px = page_pixel_size(pages_pt_[i], scale_q_);
  const double x = options_.margin + (max_page_width_px_ - px.w) / 2.0;
  return {x, tops_[i], px.w, px.h};
}

double PageLayout::total_height() const noexcept {
  if (pages_pt_.empty()) return 0.0;
  return tops_.back() - options_.gap + options_.margin;
}

double PageLayout::document_width() const noexcept {
  return max_page_width_px_ + 2.0 * options_.margin;
}

int PageLayout::page_at(double doc_y) const noexcept {
  if (pages_pt_.empty()) return -1;
  // tops_[i] <= y < tops_[i+1]  →  i. 범위 밖은 clamp
  auto it = std::upper_bound(tops_.begin(), tops_.end() - 1, doc_y);
  auto idx = static_cast<long>(it - tops_.begin()) - 1;
  idx = std::clamp<long>(idx, 0, static_cast<long>(pages_pt_.size()) - 1);
  return static_cast<int>(idx);
}

std::vector<int> PageLayout::pages_in(const Rect& viewport_doc) const {
  std::vector<int> out;
  if (pages_pt_.empty() || viewport_doc.empty()) return out;
  const int first = page_at(viewport_doc.y);
  const int last = page_at(viewport_doc.bottom());
  for (int p = first; p <= last; ++p) {
    if (page_rect(p).intersects(viewport_doc)) out.push_back(p);
  }
  return out;
}

Point PageLayout::doc_to_page_px(int page, Point doc) const {
  const Rect r = page_rect(page);
  return {doc.x - r.x, doc.y - r.y};
}

Point PageLayout::page_px_to_doc(int page, Point page_px) const {
  const Rect r = page_rect(page);
  return {page_px.x + r.x, page_px.y + r.y};
}

Point PageLayout::page_px_to_pt(int page, Point page_px) const {
  (void)page;
  const double s = scale();
  return {page_px.x / s, page_px.y / s};
}

namespace {
// 맞춤 배율은 내림으로 양자화한다. 반올림하면 문서가 뷰포트보다 최대 1.6% 커져 불필요한 스크롤바가 생긴다.
// 픽셀 크기는 올림(ceil)이므로 1px 여유를 둔다.
int quantize_floor(double scale) noexcept {
  const long q = static_cast<long>(std::floor(scale * kScaleQuantum));
  return static_cast<int>(q < kMinScaleQ ? kMinScaleQ : q);
}
}  // namespace

int PageLayout::fit_width_scale_q(double viewport_w) const noexcept {
  if (max_page_width_pt_ <= 0.0) return kScaleQuantum;
  const double avail = std::max(1.0, viewport_w - 2.0 * options_.margin - 1.0);
  return quantize_floor(avail / max_page_width_pt_);
}

int PageLayout::fit_page_scale_q(double viewport_w, double viewport_h, int page) const noexcept {
  if (pages_pt_.empty()) return kScaleQuantum;
  const auto& pt = pages_pt_[static_cast<std::size_t>(std::clamp(page, 0, page_count() - 1))];
  const double aw = std::max(1.0, viewport_w - 2.0 * options_.margin - 1.0);
  const double ah = std::max(1.0, viewport_h - 2.0 * options_.margin - 1.0);
  return quantize_floor(std::min(aw / pt.width, ah / pt.height));
}

}  // namespace onpdf::view
