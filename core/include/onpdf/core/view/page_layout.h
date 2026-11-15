#pragma once

// 세로 연속 레이아웃. 모든 좌표는 "문서 픽셀"(배율 적용 후). 쪽은 가장 넓은 쪽 기준 가운데 정렬.
// Qt 에 의존하지 않으며 메인 스레드에서만 쓴다.
//
//   y=0 ┌─ margin ─┐
//       │  page 0  │  tops_[0]
//       │   gap    │
//       │  page 1  │  tops_[1]
//       └─ margin ─┘  total_height()

#include "onpdf/core/engine/render_types.h"
#include "onpdf/core/geometry.h"

#include <vector>

namespace onpdf::view {

struct LayoutOptions {
  double gap = 12.0;     // 쪽 사이 간격 (문서 px)
  double margin = 16.0;  // 문서 가장자리 여백 (문서 px)
};

class PageLayout {
 public:
  explicit PageLayout(std::vector<engine::PageSize> pages_pt, LayoutOptions options = {});

  void set_scale_q(int scale_q);
  [[nodiscard]] int scale_q() const noexcept { return scale_q_; }
  [[nodiscard]] double scale() const noexcept;
  [[nodiscard]] const LayoutOptions& options() const noexcept { return options_; }

  [[nodiscard]] int page_count() const noexcept { return static_cast<int>(pages_pt_.size()); }
  [[nodiscard]] engine::PageSize page_size_pt(int page) const { return pages_pt_[static_cast<std::size_t>(page)]; }
  [[nodiscard]] Rect page_rect(int page) const;        // 문서 px
  [[nodiscard]] double total_height() const noexcept;  // 여백 포함
  [[nodiscard]] double document_width() const noexcept;  // 가장 넓은 쪽 + 양쪽 여백

  // y 가 속한 쪽 (간격은 위쪽 쪽에 귀속). 범위 밖은 0 / 마지막 쪽으로 clamp. 쪽이 없으면 -1
  [[nodiscard]] int page_at(double doc_y) const noexcept;
  // 뷰포트와 교차하는 쪽 번호 (오름차순)
  [[nodiscard]] std::vector<int> pages_in(const Rect& viewport_doc) const;

  [[nodiscard]] Point doc_to_page_px(int page, Point doc) const;
  [[nodiscard]] Point page_px_to_doc(int page, Point page_px) const;
  [[nodiscard]] Point page_px_to_pt(int page, Point page_px) const;  // 쪽 포인트 (좌상단 원점, y 아래)

  // 뷰포트 너비에 가장 넓은 쪽을 맞추는 배율
  [[nodiscard]] int fit_width_scale_q(double viewport_w) const noexcept;
  // 특정 쪽 전체가 뷰포트에 들어가는 배율
  [[nodiscard]] int fit_page_scale_q(double viewport_w, double viewport_h, int page) const noexcept;

 private:
  void rebuild();

  std::vector<engine::PageSize> pages_pt_;
  LayoutOptions options_;
  int scale_q_ = 64;
  std::vector<double> tops_;  // 크기 n+1, tops_[n] = 마지막 쪽 bottom + gap
  double max_page_width_px_ = 0.0;
  double max_page_width_pt_ = 0.0;
};

}  // namespace onpdf::view
