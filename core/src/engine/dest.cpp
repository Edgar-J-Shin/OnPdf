#include "dest.h"

#include <algorithm>
#include <cmath>

namespace onpdf::engine::detail {

PageSpace PageSpace::of(FPDF_PAGE page) {
  // PDF 최대 쪽 크기는 14,400pt 이지만 악성 MediaBox 에 대비해 int 범위로 clamp
  auto scaled = [](double pt) {
    const double v = std::clamp(pt * 1000.0, 1.0, 2.0e9);
    return static_cast<int>(std::lround(v));
  };
  return {page, scaled(FPDF_GetPageWidthF(page)), scaled(FPDF_GetPageHeightF(page))};
}

Point PageSpace::to_pt(double x, double y) const {
  int dx = 0, dy = 0;
  if (!FPDF_PageToDevice(page, 0, 0, w1000, h1000, 0, x, y, &dx, &dy)) return {0.0, 0.0};
  return {dx / 1000.0, dy / 1000.0};
}

Rect PageSpace::to_rect(double left, double right, double bottom, double top) const {
  const Point p[4] = {to_pt(left, bottom), to_pt(right, bottom), to_pt(left, top), to_pt(right, top)};
  double x0 = p[0].x, y0 = p[0].y, x1 = p[0].x, y1 = p[0].y;
  for (const auto& q : p) {
    x0 = std::min(x0, q.x);
    y0 = std::min(y0, q.y);
    x1 = std::max(x1, q.x);
    y1 = std::max(y1, q.y);
  }
  return {x0, y0, x1 - x0, y1 - y0};
}

Point PageSpace::to_user(Point pt) const {
  double ux = 0.0, uy = 0.0;
  const auto dev = [](double v) { return static_cast<int>(std::lround(std::clamp(v * 1000.0, -2.0e9, 2.0e9))); };
  if (!FPDF_DeviceToPage(page, 0, 0, w1000, h1000, 0, dev(pt.x), dev(pt.y), &ux, &uy)) return {0.0, 0.0};
  return {ux, uy};
}

std::array<double, 4> PageSpace::user_rect_of(const Rect& r) const {
  const Point p[4] = {to_user({r.x, r.y}), to_user({r.right(), r.y}), to_user({r.x, r.bottom()}),
                      to_user({r.right(), r.bottom()})};
  double x0 = p[0].x, y0 = p[0].y, x1 = p[0].x, y1 = p[0].y;
  for (const auto& q : p) {
    x0 = std::min(x0, q.x);
    y0 = std::min(y0, q.y);
    x1 = std::max(x1, q.x);
    y1 = std::max(y1, q.y);
  }
  return {x0, y0, x1, y1};  // left, bottom, right, top
}

std::array<double, 6> PageSpace::user_matrix_at(Point origin_pt) const {
  // 기저 벡터는 1000pt 떨어진 점으로 재 1/1000 로 나눈다: to_user 의 0.001pt 양자화가 1.001 같은 행렬을 만들지 않게 (리뷰 I1)
  constexpr double kSpan = 1000.0;
  const Point e = to_user(origin_pt);
  const Point right = to_user({origin_pt.x + kSpan, origin_pt.y});
  const Point up = to_user({origin_pt.x, origin_pt.y - kSpan});
  const auto snap = [](double v) { return std::abs(v) < 1e-9 ? 0.0 : v; };
  return {snap((right.x - e.x) / kSpan), snap((right.y - e.y) / kSpan), snap((up.x - e.x) / kSpan), snap((up.y - e.y) / kSpan), e.x, e.y};
}

std::array<float, 8> PageSpace::quad_of(const Rect& r) const {
  const auto u = user_rect_of(r);
  const auto l = static_cast<float>(u[0]), b = static_cast<float>(u[1]), rt = static_cast<float>(u[2]),
             t = static_cast<float>(u[3]);
  return {l, t, rt, t, l, b, rt, b};
}

bool fill_location(FPDF_DOCUMENT doc, FPDF_DEST dest, view::Link& link) {
  link.page = FPDFDest_GetDestPageIndex(doc, dest);
  if (link.page < 0) return false;
  FPDF_BOOL has_x = 0, has_y = 0, has_zoom = 0;
  FS_FLOAT x = 0, y = 0, zoom = 0;
  if (FPDFDest_GetLocationInPage(dest, &has_x, &has_y, &has_zoom, &x, &y, &zoom) && has_y) {
    // 위치는 대상 쪽의 사용자 좌표 → 대상 쪽을 잠깐 열어 변환
    ScopedPage target(FPDF_LoadPage(doc, link.page));
    if (target) {
      const PageSpace space = PageSpace::of(target.get());
      link.location = space.to_pt(has_x ? x : 0.0, y);
    }
  }
  return true;
}

bool fill_target_lazy(FPDF_DOCUMENT doc, FPDF_DEST dest, view::Link& link) {
  link.kind = view::LinkKind::kPage;
  link.page = FPDFDest_GetDestPageIndex(doc, dest);
  if (link.page < 0 || link.page >= FPDF_GetPageCount(doc)) return false;
  FPDF_BOOL has_x = 0, has_y = 0, has_zoom = 0;
  FS_FLOAT x = 0, y = 0, zoom = 0;
  if (FPDFDest_GetLocationInPage(dest, &has_x, &has_y, &has_zoom, &x, &y, &zoom) && has_y) {
    link.user_location = Point{has_x ? x : 0.0, y};
  }
  return true;
}

std::string uri_of(FPDF_DOCUMENT doc, FPDF_ACTION action) {
  const unsigned long len = FPDFAction_GetURIPath(doc, action, nullptr, 0);  // NUL 포함 길이
  if (len <= 1) return {};
  std::string buf(len, '\0');
  FPDFAction_GetURIPath(doc, action, buf.data(), len);
  buf.resize(len - 1);
  return buf;
}

std::vector<unsigned short> to_wide(const std::string& utf8) {
  std::vector<unsigned short> out;
  out.reserve(utf8.size() + 1);
  auto push = [&](char32_t cp) {
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(static_cast<unsigned short>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<unsigned short>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<unsigned short>(cp));
    }
  };
  for (std::size_t i = 0; i < utf8.size();) {
    const auto c = static_cast<unsigned char>(utf8[i]);
    int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (len == 0 || i + static_cast<std::size_t>(len) > utf8.size()) {
      push(0xFFFD);
      ++i;
      continue;
    }
    char32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
    bool ok = true;
    for (int k = 1; k < len; ++k) {
      const auto cc = static_cast<unsigned char>(utf8[i + static_cast<std::size_t>(k)]);
      if ((cc & 0xC0) != 0x80) {
        ok = false;
        break;
      }
      cp = (cp << 6) | (cc & 0x3F);
    }
    // overlong(C0 80 → U+0000 등)은 거부: NUL 이 쿼리를 잘라 버린다
    static constexpr char32_t kMin[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (!ok || cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      push(0xFFFD);
      ++i;
      continue;
    }
    push(cp);
    i += static_cast<std::size_t>(len);
  }
  out.push_back(0);
  return out;
}

std::string utf16le_to_utf8(const unsigned char* data, std::size_t bytes) {
  std::string out;
  if (data == nullptr) return out;
  const std::size_t units = bytes / 2;
  std::vector<char16_t> u(units);
  for (std::size_t i = 0; i < units; ++i) {
    u[i] = static_cast<char16_t>(data[2 * i] | (data[2 * i + 1] << 8));
  }
  std::size_t n = units;
  while (n > 0 && u[n - 1] == 0) --n;  // 끝 NUL
  for (std::size_t i = 0; i < n; ++i) {
    char32_t cp = u[i];
    if (cp >= 0xD800 && cp <= 0xDBFF) {
      if (i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00);
        ++i;
      } else {
        cp = 0xFFFD;
      }
    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
      cp = 0xFFFD;
    }
    view::append_utf8(out, cp);
  }
  if (bytes % 2 != 0) view::append_utf8(out, 0xFFFD);  // 홀수 바이트
  return out;
}

}  // namespace onpdf::engine::detail
