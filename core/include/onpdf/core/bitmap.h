#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace onpdf {

// PDFium 이 그리는 그대로의 BGRA 8888 비트맵 (알파 미적용, 좌상단 원점).
// 리틀엔디언에서 QImage::Format_ARGB32 와 메모리 배치가 같다.
struct Bitmap {
  int width = 0;
  int height = 0;
  int stride = 0;  // 한 줄의 바이트 수 (>= width * 4)
  std::vector<std::uint8_t> pixels;

  [[nodiscard]] bool empty() const noexcept { return width <= 0 || height <= 0; }

  [[nodiscard]] const std::uint8_t* row(int y) const noexcept {
    return pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
  }
};

}  // namespace onpdf
