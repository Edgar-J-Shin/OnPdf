#pragma once

// 인쇄 순수 로직: 쪽 선택·매수 순서·용지 배치·렌더 해상도. Qt·PDFium 에 의존하지 않는다.
// 좌표 단위는 포인트(1/72 inch). 배치 사각형은 인쇄 가능 영역의 좌상단 기준.

#include "onpdf/core/engine/render_types.h"
#include "onpdf/core/geometry.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace onpdf::view {

enum class PrintScaling {
  kFit,              // 인쇄 가능 영역에 맞춰 확대·축소
  kActualSize,       // 100%. 용지보다 크면 가운데 기준으로 잘린다
  kShrinkOversized,  // 큰 쪽만 축소, 작은 쪽은 100%
};

enum class Duplex { kUndefined, kSimplex, kFlipShortEdge, kFlipLongEdge };

// 문서가 정한 인쇄 권한·기본값. 워커가 문서를 열 때 한 번 채운다.
struct PrintPreferences {
  bool allowed = true;                           // 권한 비트 3 (인쇄)
  bool high_quality = true;                      // 권한 비트 12 (고품질 인쇄). 거짓이면 저해상도만
  bool no_scaling = false;                       // /ViewerPreferences /PrintScaling /None
  int copies = 1;                                // /NumCopies (1~999)
  std::vector<std::pair<int, int>> page_ranges;  // /PrintPageRange, 0 기준 닫힌 구간. 비면 전체
  Duplex duplex = Duplex::kUndefined;
};

struct PageRange {
  int first = 0;  // 0 기준, 닫힌 구간
  int last = 0;
};

// 범위 목록 → 출력할 쪽 순서. 쪽 수 밖은 잘라내고 뒤집힌 구간은 버린다. 겹침·중복은 사용자 의도로
// 보고 유지
[[nodiscard]] std::vector<int> pages_in_ranges(const std::vector<PageRange>& ranges,
                                               int page_count);

struct PrintStep {
  int page = 0;
  int repeat = 1;  // 같은 쪽을 연속으로 찍을 장 수 (한 부씩 인쇄가 아닐 때 매수)
};

// 매수 확장. collate=true 면 전체 순서를 copies 번 반복, false 면 쪽마다 repeat=copies
[[nodiscard]] std::vector<PrintStep> print_sequence(const std::vector<int>& pages, int copies,
                                                    bool collate);

struct Placement {
  int rotation = 0;    // 렌더 회전: 0 또는 270 (반시계 90°, 프린터 가로 방향 관례)
  double scale = 1.0;  // 쪽 포인트 → 용지 포인트
  Rect target;  // 인쇄 가능 영역 좌상단 기준 포인트. 실제 크기에서는 영역보다 클 수 있다(잘림)
};

// 쪽을 인쇄 가능 영역에 배치한다. 쪽과 영역의 가로·세로 방향이 다르면(auto_rotate) 회전한다.
// 크기가 0 이하이거나 유한하지 않으면 nullopt
[[nodiscard]] std::optional<Placement> place_page(engine::PageSize page_pt,
                                                  engine::PageSize printable_pt,
                                                  PrintScaling scaling, bool auto_rotate = true);

inline constexpr int kMaxPrintDpi = 300;
inline constexpr int kLowQualityPrintDpi = 150;
inline constexpr int kMinPrintDpi = 72;
inline constexpr std::int64_t kMaxPrintPixels = 16LL * 1024 * 1024;  // 쪽 비트맵 상한 (BGRA 64MB)

// 배치된 쪽(target, 포인트)을 렌더할 DPI. 프린터 해상도와 상한(300, 저품질 150) 중 작은 값,
// 비트맵이 kMaxPrintPixels 를 넘으면 낮춘다. 하한 72
[[nodiscard]] int choose_print_dpi(int printer_dpi, bool high_quality, const Rect& target_pt);

}  // namespace onpdf::view
