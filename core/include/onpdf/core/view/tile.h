#pragma once

// 타일 키와 격자 계산. 타일은 "쪽을 배율 s 로 렌더한 픽셀 평면" 을 512×512 로 나눈 조각이다.
// 배율은 1/64 단위로 양자화해 키 충돌과 부동소수 오차를 막는다. 뷰는 양자화된 배율로 레이아웃을
// 계산하므로 그리기 배율과 타일 배율이 항상 같다.

#include "onpdf/core/engine/render_types.h"
#include "onpdf/core/geometry.h"

#include <cmath>
#include <compare>
#include <cstddef>
#include <functional>

namespace onpdf::view {

constexpr int kTileSize = 512;
constexpr int kScaleQuantum = 64;
constexpr int kMinScaleQ = 1;  // 1/64 px/pt

[[nodiscard]] inline int quantize_scale(double scale) noexcept {
  const long q = std::lround(scale * kScaleQuantum);
  return static_cast<int>(q < kMinScaleQ ? kMinScaleQ : q);
}

[[nodiscard]] inline double dequantize_scale(int scale_q) noexcept {
  return static_cast<double>(scale_q) / kScaleQuantum;
}

struct TileKey {
  int page = 0;
  int scale_q = 0;
  int tx = 0;
  int ty = 0;
  auto operator<=>(const TileKey&) const = default;
};

struct TileKeyHash {
  std::size_t operator()(const TileKey& k) const noexcept {
    std::size_t h = static_cast<std::size_t>(k.page) * 73856093u;
    h ^= static_cast<std::size_t>(k.scale_q) * 19349663u;
    h ^= static_cast<std::size_t>(k.tx) * 83492791u;
    h ^= static_cast<std::size_t>(k.ty) * 2654435761u;
    return h;
  }
};

// 쪽 크기(포인트) 를 배율로 렌더했을 때의 픽셀 크기 (올림)
[[nodiscard]] inline Size page_pixel_size(engine::PageSize pt, int scale_q) noexcept {
  const double s = dequantize_scale(scale_q);
  return {std::ceil(pt.width * s), std::ceil(pt.height * s)};
}

[[nodiscard]] inline int tiles_across(double px) noexcept {
  return px <= 0.0 ? 0 : static_cast<int>(std::ceil(px / kTileSize));
}

// 타일의 쪽 픽셀 사각형. 마지막 행·열은 쪽 경계에서 잘린다.
[[nodiscard]] inline Rect tile_rect_px(const TileKey& key, engine::PageSize pt) noexcept {
  const Size page = page_pixel_size(pt, key.scale_q);
  const double x = static_cast<double>(key.tx) * kTileSize;
  const double y = static_cast<double>(key.ty) * kTileSize;
  return {x, y, std::min<double>(kTileSize, page.w - x), std::min<double>(kTileSize, page.h - y)};
}

}  // namespace onpdf::view
