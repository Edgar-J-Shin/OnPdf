#pragma once

// 렌더 위험(#121): PDFium 이 끝내지 못하는 입력을 렌더 전에 걸러 낸다. 지금 아는 종류는 타일링
// 패턴의 극단적으로 작은 /XStep·/YStep(pdf.js tiling-pattern-tiny-steps*: 0.001pt → 쪽당 10^12
// 타일, FPDF_RenderPageBitmap 이 돌아오지 않고 progressive render 의 pause 도 객체 사이에서만
// 확인된다). 검사는 QpdfTools::scan_render_hazards(qpdf), 판정은 PdfiumDocument 가 쪽별로 캐시해
// render_page/render_region 을 kRenderRefused 로 즉시 거절한다. PDFium·qpdf 헤더 없음.

#include <string>

namespace onpdf::engine {

enum class RenderHazardKind {
  kTilingPatternTooDense,   // 쪽당 추정 타일 수 > kMaxPatternTilesPerPage
  kTilingPatternDegenerate  // 셀 넓이·행렬식이 0 이거나 비유한
};

struct RenderHazard {
  int page = 0;  // 0 기준
  RenderHazardKind kind = RenderHazardKind::kTilingPatternTooDense;
  double estimate = 0.0;  // 쪽당 추정 타일 수 (degenerate 면 0)
  std::string detail;     // 사람이 읽는 사유 (오류 메시지에 그대로)
};

// 쪽당 타일 수 문턱. 코퍼스 측정(2026-11): 정상 문서 최대 4.85e3(10pt 스텝), 폭탄 4.85e11.
// 0.5pt 스텝 A4 도 2e6 이라 여유가 있다
inline constexpr double kMaxPatternTilesPerPage = 1e7;

inline const char* to_string(RenderHazardKind kind) noexcept {
  switch (kind) {
    case RenderHazardKind::kTilingPatternTooDense:
      return "tiling pattern too dense";
    case RenderHazardKind::kTilingPatternDegenerate:
      return "degenerate tiling pattern";
  }
  return "unknown";
}

}  // namespace onpdf::engine
