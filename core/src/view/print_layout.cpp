#include "onpdf/core/view/print_layout.h"

#include <algorithm>
#include <cmath>

namespace onpdf::view {

std::vector<int> pages_in_ranges(const std::vector<PageRange>& ranges, int page_count) {
  std::vector<int> pages;
  if (page_count <= 0) return pages;
  for (const auto& range : ranges) {
    if (range.last < range.first) continue;
    const int first = std::max(range.first, 0);
    const int last = std::min(range.last, page_count - 1);
    for (int p = first; p <= last; ++p) pages.push_back(p);
  }
  return pages;
}

std::vector<PrintStep> print_sequence(const std::vector<int>& pages, int copies, bool collate) {
  std::vector<PrintStep> steps;
  copies = std::max(copies, 1);
  if (collate) {
    steps.reserve(pages.size() * static_cast<std::size_t>(copies));
    for (int c = 0; c < copies; ++c) {
      for (int page : pages) steps.push_back({page, 1});
    }
  } else {
    steps.reserve(pages.size());
    for (int page : pages) steps.push_back({page, copies});
  }
  return steps;
}

namespace {

bool valid_size(engine::PageSize size) {
  return std::isfinite(size.width) && std::isfinite(size.height) && size.width > 0.0 && size.height > 0.0;
}

}  // namespace

std::optional<Placement> place_page(engine::PageSize page_pt, engine::PageSize printable_pt, PrintScaling scaling,
                                    bool auto_rotate) {
  if (!valid_size(page_pt) || !valid_size(printable_pt)) return std::nullopt;
  Placement out;
  const bool page_landscape = page_pt.width > page_pt.height;
  const bool paper_landscape = printable_pt.width > printable_pt.height;
  // 정사각형 쪽·용지는 방향이 없으므로 회전하지 않는다
  const bool square = page_pt.width == page_pt.height || printable_pt.width == printable_pt.height;
  if (auto_rotate && !square && page_landscape != paper_landscape) {
    out.rotation = 270;
    std::swap(page_pt.width, page_pt.height);
  }
  const double fit = std::min(printable_pt.width / page_pt.width, printable_pt.height / page_pt.height);
  switch (scaling) {
    case PrintScaling::kFit: out.scale = fit; break;
    case PrintScaling::kActualSize: out.scale = 1.0; break;
    case PrintScaling::kShrinkOversized: out.scale = std::min(1.0, fit); break;
  }
  const double w = page_pt.width * out.scale;
  const double h = page_pt.height * out.scale;
  out.target = Rect{(printable_pt.width - w) / 2.0, (printable_pt.height - h) / 2.0, w, h};
  return out;
}

int choose_print_dpi(int printer_dpi, bool high_quality, const Rect& target_pt) {
  int dpi = printer_dpi > 0 ? printer_dpi : kMaxPrintDpi;
  dpi = std::min(dpi, high_quality ? kMaxPrintDpi : kLowQualityPrintDpi);
  const double area_in2 = (target_pt.w / 72.0) * (target_pt.h / 72.0);
  if (area_in2 > 0.0 && std::isfinite(area_in2)) {
    const double pixels = area_in2 * static_cast<double>(dpi) * static_cast<double>(dpi);
    if (pixels > static_cast<double>(kMaxPrintPixels)) {
      dpi = static_cast<int>(std::floor(std::sqrt(static_cast<double>(kMaxPrintPixels) / area_in2)));
    }
  }
  return std::max(dpi, kMinPrintDpi);
}

}  // namespace onpdf::view
