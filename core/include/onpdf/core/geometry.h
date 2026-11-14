#pragma once

// Qt 에 의존하지 않는 기하 값 타입. 좌표 단위는 문맥(포인트/픽셀)에 따른다.

#include <algorithm>

namespace onpdf {

struct Point {
  double x = 0.0;
  double y = 0.0;
};

struct Size {
  double w = 0.0;
  double h = 0.0;
};

struct Rect {
  double x = 0.0;
  double y = 0.0;
  double w = 0.0;
  double h = 0.0;

  [[nodiscard]] double right() const noexcept { return x + w; }
  [[nodiscard]] double bottom() const noexcept { return y + h; }
  [[nodiscard]] bool empty() const noexcept { return w <= 0.0 || h <= 0.0; }

  [[nodiscard]] bool contains(Point p) const noexcept {
    return p.x >= x && p.x < right() && p.y >= y && p.y < bottom();
  }
  [[nodiscard]] bool intersects(const Rect& o) const noexcept {
    return !empty() && !o.empty() && x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom();
  }
  // 교집합. 비어 있으면 {0,0,0,0}
  [[nodiscard]] Rect intersected(const Rect& o) const noexcept {
    const double nx = std::max(x, o.x);
    const double ny = std::max(y, o.y);
    const double nr = std::min(right(), o.right());
    const double nb = std::min(bottom(), o.bottom());
    if (nr <= nx || nb <= ny) return {};
    return {nx, ny, nr - nx, nb - ny};
  }
  [[nodiscard]] Rect translated(double dx, double dy) const noexcept { return {x + dx, y + dy, w, h}; }
  [[nodiscard]] bool operator==(const Rect&) const = default;
};

}  // namespace onpdf
