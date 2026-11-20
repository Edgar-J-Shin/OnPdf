// Spike #55: 기존 텍스트 줄 단위 수정 가능성 검증 (M4 사전 검증).
// 제품 코드가 아니다 — PDFium 헤더를 engine 밖에서 직접 쓴다(스파이크 예외). 결론만 01-spike.md 로 옮긴다.
//
// 질문: PDFium 공개 API 만으로 실제 문서의 기존 텍스트를 줄로 묶고, 한 줄의 객체를 지운 뒤 같은 자리·크기·색으로
//       새 텍스트를 넣었을 때, 나머지 내용이 그대로 보존되고 결과가 원본과 어울리는가?
//
// 실행: onpdf_spike_text_edit <out-dir> [--pages N] [--scale S] [--font <cid-ttf>] a.pdf b.pdf ...
// 출력: <out-dir>/pages.tsv (쪽별 줄 묶기·재생성 충실도), lines.tsv (대상 줄별 삭제·교체 결과),
//       fonts.tsv (글꼴별 글자 커버리지), <doc>/p<N>-l<K>.png (원본 / 제자리 SetText / 대체 글꼴 비교)
//
// 실험
//   E0 재생성 충실도: 아무것도 지우지 않고 텍스트 객체 하나를 더럽혀(항등 변환) GenerateContent → 저장 → 다시 열기 → 쪽 전체 픽셀 비교
//   E1 줄 묶기: 텍스트 객체(Form XObject 안 포함)를 기준선·방향·간격으로 묶어 FPDFText 추출 줄(FPDFText_GetTextObject)과 비교
//   E2 줄 삭제: 줄 객체 삭제 → GenerateContent → 저장 → 다시 열기 → 줄 영역 밖 픽셀 차이, qpdf check
//   E3a 제자리 수정: 줄 첫 객체에 FPDFText_SetText(줄 전체 문자열), 나머지 삭제 → 폭·위치·픽셀 비교 (원본 글꼴 재사용)
//   E3b 대체 글꼴: 줄 객체 삭제 → 표준 14 글꼴(라틴) 또는 CID TrueType(한글)로 새 객체 → 폭·픽셀 비교
//   E4 글꼴 커버리지: 원본 글꼴에 숫자·영문·한글 상용 음절의 글리프가 있는가 (FPDFFont_GetGlyphPath, .notdef 서명 비교)
#include "onpdf/core/bitmap.h"
#include "onpdf/core/engine/qpdf_tools.h"
#include "onpdf/core/image/png_io.h"
#include "onpdf/core/platform/path.h"
#include "text_edit_surgery.h"

#include <fpdf_doc.h>
#include <fpdf_edit.h>
#include <fpdf_save.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// ---------- 공용 ----------

std::vector<std::uint8_t> read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

void append_utf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

std::u32string utf16_to_u32(const std::vector<unsigned short>& w) {
  std::u32string out;
  for (std::size_t i = 0; i < w.size(); ++i) {
    char32_t c = w[i];
    if (c == 0) break;
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < w.size()) {
      c = 0x10000 + ((c - 0xD800) << 10) + (w[i + 1] - 0xDC00);
      ++i;
    }
    out += c;
  }
  return out;
}

std::vector<unsigned short> u32_to_utf16z(const std::u32string& s) {
  std::vector<unsigned short> out;
  for (char32_t c : s) {
    if (c >= 0x10000) {
      c -= 0x10000;
      out.push_back(static_cast<unsigned short>(0xD800 + (c >> 10)));
      out.push_back(static_cast<unsigned short>(0xDC00 + (c & 0x3FF)));
    } else {
      out.push_back(static_cast<unsigned short>(c));
    }
  }
  out.push_back(0);
  return out;
}

std::string u8(const std::u32string& s) {
  std::string out;
  for (char32_t c : s) {
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    append_utf8(out, c);
  }
  return out;
}

bool is_hangul(char32_t c) { return (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F); }
bool has_hangul(const std::u32string& s) { return std::any_of(s.begin(), s.end(), is_hangul); }
bool is_space(char32_t c) { return c == ' ' || c == 0xA0 || c == '\t' || c == 0x3000; }

std::u32string trim(const std::u32string& s) {
  std::size_t b = 0, e = s.size();
  while (b < e && is_space(s[b])) ++b;
  while (e > b && is_space(s[e - 1])) --e;
  return s.substr(b, e - b);
}

std::string fmt(double v, int prec = 2) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", prec, v);
  return buf;
}

struct Mat {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};
// PDF 행 벡터 규약: p' = p × A × B
Mat mul(const Mat& A, const Mat& B) {
  return {A.a * B.a + A.b * B.c, A.a * B.b + A.b * B.d, A.c * B.a + A.d * B.c,
          A.c * B.b + A.d * B.d, A.e * B.a + A.f * B.c + B.e, A.e * B.b + A.f * B.d + B.f};
}
void apply(const Mat& m, double x, double y, double& ox, double& oy) {
  ox = x * m.a + y * m.c + m.e;
  oy = x * m.b + y * m.d + m.f;
}

struct Rect {
  double l = 1e18, b = 1e18, r = -1e18, t = -1e18;
  [[nodiscard]] bool valid() const { return r >= l && t >= b; }
  void add(double x, double y) {
    l = std::min(l, x); r = std::max(r, x);
    b = std::min(b, y); t = std::max(t, y);
  }
  void add(const Rect& o) {
    if (!o.valid()) return;
    add(o.l, o.b);
    add(o.r, o.t);
  }
};

Mat get_matrix(FPDF_PAGEOBJECT obj) {
  FS_MATRIX m{};
  if (!FPDFPageObj_GetMatrix(obj, &m)) return {};
  return {m.a, m.b, m.c, m.d, m.e, m.f};
}

// 컨테이너(쪽 또는 Form) 좌표의 경계 → 쪽 사용자 좌표
Rect bounds_in_page(FPDF_PAGEOBJECT obj, const Mat& container) {
  float l = 0, b = 0, r = 0, t = 0;
  Rect out;
  if (!FPDFPageObj_GetBounds(obj, &l, &b, &r, &t)) return out;
  const double xs[2] = {l, r}, ys[2] = {b, t};
  for (double x : xs)
    for (double y : ys) {
      double px = 0, py = 0;
      apply(container, x, y, px, py);
      out.add(px, py);
    }
  return out;
}

// ---------- 메모리 저장 ----------

struct MemWriter : FPDF_FILEWRITE {
  std::vector<std::uint8_t> data;
};
int write_block(FPDF_FILEWRITE* self, const void* buf, unsigned long size) {
  auto* w = static_cast<MemWriter*>(self);
  const auto* p = static_cast<const std::uint8_t*>(buf);
  w->data.insert(w->data.end(), p, p + size);
  return 1;
}
std::optional<std::vector<std::uint8_t>> save_copy(FPDF_DOCUMENT doc) {
  MemWriter w{};
  w.version = 1;
  w.WriteBlock = &write_block;
  if (!FPDF_SaveAsCopy(doc, &w, FPDF_NO_INCREMENTAL)) return std::nullopt;
  return std::move(w.data);
}

// ---------- RAII ----------

struct DocCloser { void operator()(FPDF_DOCUMENT d) const { FPDF_CloseDocument(d); } };
struct PageCloser { void operator()(FPDF_PAGE p) const { FPDF_ClosePage(p); } };
struct TextCloser { void operator()(FPDF_TEXTPAGE t) const { FPDFText_ClosePage(t); } };
struct BmpCloser { void operator()(FPDF_BITMAP b) const { FPDFBitmap_Destroy(b); } };
using DocPtr = std::unique_ptr<std::remove_pointer_t<FPDF_DOCUMENT>, DocCloser>;
using PagePtr = std::unique_ptr<std::remove_pointer_t<FPDF_PAGE>, PageCloser>;
using TextPtr = std::unique_ptr<std::remove_pointer_t<FPDF_TEXTPAGE>, TextCloser>;
using BmpPtr = std::unique_ptr<std::remove_pointer_t<FPDF_BITMAP>, BmpCloser>;

// 문서는 바이트 버퍼보다 먼저 닫혀야 한다
struct LoadedDoc {
  std::shared_ptr<std::vector<std::uint8_t>> bytes;
  DocPtr doc;
};
std::optional<LoadedDoc> load(std::shared_ptr<std::vector<std::uint8_t>> bytes) {
  LoadedDoc ld{std::move(bytes), nullptr};
  ld.doc.reset(FPDF_LoadMemDocument64(ld.bytes->data(), ld.bytes->size(), nullptr));
  if (!ld.doc) return std::nullopt;
  return ld;
}

// ---------- 렌더·비교 ----------

onpdf::Bitmap render(FPDF_PAGE page, double scale) {
  const int w = std::max(1, static_cast<int>(std::lround(FPDF_GetPageWidthF(page) * scale)));
  const int h = std::max(1, static_cast<int>(std::lround(FPDF_GetPageHeightF(page) * scale)));
  BmpPtr bmp(FPDFBitmap_Create(w, h, 0));
  onpdf::Bitmap out;
  if (!bmp) return out;
  FPDFBitmap_FillRect(bmp.get(), 0, 0, w, h, 0xFFFFFFFF);
  FPDF_RenderPageBitmap(bmp.get(), page, 0, 0, w, h, 0, 0);
  out.width = w;
  out.height = h;
  out.stride = w * 4;
  out.pixels.resize(static_cast<std::size_t>(out.stride) * static_cast<std::size_t>(h));
  const auto* src = static_cast<const std::uint8_t*>(FPDFBitmap_GetBuffer(bmp.get()));
  const int sstride = FPDFBitmap_GetStride(bmp.get());
  for (int y = 0; y < h; ++y) {
    std::copy_n(src + static_cast<std::size_t>(y) * static_cast<std::size_t>(sstride), static_cast<std::size_t>(w) * 4,
                out.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(out.stride));
    for (int x = 0; x < w; ++x) out.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(out.stride) + static_cast<std::size_t>(x) * 4 + 3] = 255;
  }
  return out;
}

struct PixRect {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // [x0,x1) × [y0,y1)
  [[nodiscard]] bool contains(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
  [[nodiscard]] long area() const { return static_cast<long>(std::max(0, x1 - x0)) * std::max(0, y1 - y0); }
};

PixRect to_device(FPDF_PAGE page, const Rect& r, const onpdf::Bitmap& bmp, int pad) {
  PixRect out{bmp.width, bmp.height, 0, 0};
  const double xs[2] = {r.l, r.r}, ys[2] = {r.b, r.t};
  for (double x : xs)
    for (double y : ys) {
      int dx = 0, dy = 0;
      FPDF_PageToDevice(page, 0, 0, bmp.width, bmp.height, 0, x, y, &dx, &dy);
      out.x0 = std::min(out.x0, dx);
      out.y0 = std::min(out.y0, dy);
      out.x1 = std::max(out.x1, dx);
      out.y1 = std::max(out.y1, dy);
    }
  out.x0 = std::max(0, out.x0 - pad);
  out.y0 = std::max(0, out.y0 - pad);
  out.x1 = std::min(bmp.width, out.x1 + pad);
  out.y1 = std::min(bmp.height, out.y1 + pad);
  return out;
}

int px_diff(const onpdf::Bitmap& a, const onpdf::Bitmap& b, int x, int y) {
  const auto* pa = a.row(y) + x * 4;
  const auto* pb = b.row(y) + x * 4;
  return std::max({std::abs(pa[0] - pb[0]), std::abs(pa[1] - pb[1]), std::abs(pa[2] - pb[2])});
}

struct DiffStats {
  long outside = 0;      // 마스크 밖에서 차이 > 0 인 픽셀
  long outside_big = 0;  // 마스크 밖에서 차이 > 48
  long inside = 0;       // 마스크 안에서 차이 > 48
  long mask_area = 0;
  bool size_mismatch = false;
};

DiffStats diff(const onpdf::Bitmap& a, const onpdf::Bitmap& b, const std::optional<PixRect>& mask) {
  DiffStats s;
  if (a.width != b.width || a.height != b.height) {
    s.size_mismatch = true;
    return s;
  }
  if (mask) s.mask_area = mask->area();
  for (int y = 0; y < a.height; ++y)
    for (int x = 0; x < a.width; ++x) {
      const int d = px_diff(a, b, x, y);
      if (d == 0) continue;
      if (mask && mask->contains(x, y)) {
        if (d > 48) ++s.inside;
      } else {
        ++s.outside;
        if (d > 48) ++s.outside_big;
      }
    }
  return s;
}

// 차이 시각화: 원본을 옅게, 다른 픽셀을 빨강으로
void write_diff_png(const onpdf::Bitmap& a, const onpdf::Bitmap& b, const fs::path& out) {
  if (a.width != b.width || a.height != b.height) return;
  onpdf::Bitmap v = a;
  for (int y = 0; y < a.height; ++y)
    for (int x = 0; x < a.width; ++x) {
      auto* p = v.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(v.stride) + static_cast<std::size_t>(x) * 4;
      if (px_diff(a, b, x, y) > 0) {
        p[0] = 0; p[1] = 0; p[2] = 255;
      } else {
        for (int k = 0; k < 3; ++k) p[k] = static_cast<std::uint8_t>(255 - (255 - p[k]) / 4);
      }
    }
  (void)onpdf::image::write_png(v, out);
}

onpdf::Bitmap crop(const onpdf::Bitmap& src, const PixRect& r) {
  onpdf::Bitmap out;
  out.width = std::max(1, r.x1 - r.x0);
  out.height = std::max(1, r.y1 - r.y0);
  out.stride = out.width * 4;
  out.pixels.assign(static_cast<std::size_t>(out.stride) * static_cast<std::size_t>(out.height), 255);
  if (src.width == 0) return out;
  for (int y = 0; y < out.height && r.y0 + y < src.height; ++y)
    std::copy_n(src.row(r.y0 + y) + r.x0 * 4, static_cast<std::size_t>(std::min(out.width, src.width - r.x0)) * 4,
                out.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(out.stride));
  return out;
}

onpdf::Bitmap stack(const std::vector<onpdf::Bitmap>& parts) {
  onpdf::Bitmap out;
  for (const auto& p : parts) {
    out.width = std::max(out.width, p.width);
    out.height += p.height + 3;
  }
  out.stride = out.width * 4;
  out.pixels.assign(static_cast<std::size_t>(out.stride) * static_cast<std::size_t>(out.height), 255);
  int y0 = 0;
  for (const auto& p : parts) {
    for (int y = 0; y < p.height; ++y)
      std::copy_n(p.row(y), static_cast<std::size_t>(p.width) * 4,
                  out.pixels.data() + static_cast<std::size_t>(y0 + y) * static_cast<std::size_t>(out.stride));
    y0 += p.height;
    for (int y = y0; y < y0 + 3 && y < out.height; ++y)
      for (int x = 0; x < out.width; ++x) {
        auto* q = out.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(out.stride) + static_cast<std::size_t>(x) * 4;
        q[0] = 200; q[1] = 120; q[2] = 40;
      }
    y0 += 3;
  }
  return out;
}

// ---------- 텍스트 객체 수집 ----------

struct TObj {
  std::vector<int> path;  // 쪽 객체 index, Form 안이면 [form index, 안쪽 index, ...]
  FPDF_PAGEOBJECT handle = nullptr;
  FPDF_PAGEOBJECT parent_form = nullptr;  // 쪽 직속이면 nullptr
  Mat container;                          // 컨테이너 → 쪽 사용자 좌표
  Mat m;                                  // 텍스트 행렬(쪽 사용자 좌표)
  Rect bounds;                            // 쪽 사용자 좌표
  FPDF_FONT font = nullptr;
  float size = 0;
  double eff_size = 0;  // 쪽 좌표에서의 글자 높이
  double dir_x = 1, dir_y = 0;
  std::u32string text;
  unsigned r = 0, g = 0, b = 0, a = 255;
  int render_mode = 0;
  int depth = 0;
};

std::u32string obj_text(FPDF_PAGEOBJECT obj, FPDF_TEXTPAGE tp) {
  const unsigned long n = FPDFTextObj_GetText(obj, tp, nullptr, 0);
  if (n == 0) return {};
  std::vector<unsigned short> buf(n / 2 + 1, 0);
  FPDFTextObj_GetText(obj, tp, reinterpret_cast<FPDF_WCHAR*>(buf.data()), n);
  return utf16_to_u32(buf);
}

struct Collector {
  FPDF_TEXTPAGE tp = nullptr;
  std::vector<TObj> objs;
  int forms = 0, shadings = 0, images = 0, paths = 0;

  void visit(FPDF_PAGEOBJECT obj, std::vector<int> path, FPDF_PAGEOBJECT parent, const Mat& container, int depth) {
    const int type = FPDFPageObj_GetType(obj);
    if (type == FPDF_PAGEOBJ_SHADING) ++shadings;
    if (type == FPDF_PAGEOBJ_IMAGE) ++images;
    if (type == FPDF_PAGEOBJ_PATH) ++paths;
    if (type == FPDF_PAGEOBJ_FORM) {
      ++forms;
      if (depth > 8) return;
      const Mat inner = mul(get_matrix(obj), container);
      const int n = FPDFFormObj_CountObjects(obj);
      for (int i = 0; i < n; ++i) {
        auto p = path;
        p.push_back(i);
        if (FPDF_PAGEOBJECT child = FPDFFormObj_GetObject(obj, static_cast<unsigned long>(i)))
          visit(child, std::move(p), obj, inner, depth + 1);
      }
      return;
    }
    if (type != FPDF_PAGEOBJ_TEXT) return;
    TObj t;
    t.path = std::move(path);
    t.handle = obj;
    t.parent_form = parent;
    t.container = container;
    t.m = mul(get_matrix(obj), container);
    t.bounds = bounds_in_page(obj, container);
    t.font = FPDFTextObj_GetFont(obj);
    FPDFTextObj_GetFontSize(obj, &t.size);
    const double hx = std::hypot(t.m.a, t.m.b), vy = std::hypot(t.m.c, t.m.d);
    t.eff_size = t.size * (vy > 0 ? vy : 1.0);
    if (hx > 0) {
      t.dir_x = t.m.a / hx;
      t.dir_y = t.m.b / hx;
    }
    t.text = obj_text(obj, tp);
    FPDFPageObj_GetFillColor(obj, &t.r, &t.g, &t.b, &t.a);
    t.render_mode = FPDFTextObj_GetTextRenderMode(obj);
    t.depth = depth;
    objs.push_back(std::move(t));
  }

  void run(FPDF_PAGE page) {
    const int n = FPDFPage_CountObjects(page);
    for (int i = 0; i < n; ++i)
      if (FPDF_PAGEOBJECT o = FPDFPage_GetObject(page, i)) visit(o, {i}, nullptr, Mat{}, 0);
  }
};

FPDF_PAGEOBJECT resolve(FPDF_PAGE page, const std::vector<int>& path, FPDF_PAGEOBJECT* parent) {
  *parent = nullptr;
  FPDF_PAGEOBJECT cur = FPDFPage_GetObject(page, path[0]);
  for (std::size_t k = 1; cur && k < path.size(); ++k) {
    *parent = cur;
    cur = FPDFFormObj_GetObject(cur, static_cast<unsigned long>(path[k]));
  }
  return cur;
}

// ---------- 줄 묶기 ----------

struct Line {
  std::vector<int> members;  // objs index, 기준선 방향 순서
  double baseline = 0;       // 법선 방향 투영
  double along0 = 0, along1 = 0;
  double dir_x = 1, dir_y = 0;
  double eff_size = 0;
  Rect bounds;
  std::set<FPDF_FONT> fonts;
  std::set<float> sizes;
};

double along_of(const TObj& o, double x, double y) { return x * o.dir_x + y * o.dir_y; }
double normal_of(const TObj& o) { return -o.m.e * o.dir_y + o.m.f * o.dir_x; }

std::pair<double, double> along_range(const TObj& o) {
  if (!o.bounds.valid()) return {along_of(o, o.m.e, o.m.f), along_of(o, o.m.e, o.m.f)};
  double lo = 1e18, hi = -1e18;
  const double xs[2] = {o.bounds.l, o.bounds.r}, ys[2] = {o.bounds.b, o.bounds.t};
  for (double x : xs)
    for (double y : ys) {
      const double v = along_of(o, x, y);
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
  return {lo, hi};
}

std::vector<Line> group_lines(const std::vector<TObj>& objs) {
  std::vector<int> idx;
  for (int i = 0; i < static_cast<int>(objs.size()); ++i)
    if (!trim(objs[static_cast<std::size_t>(i)].text).empty() && objs[static_cast<std::size_t>(i)].eff_size > 0.1) idx.push_back(i);
  // 방향(1도 단위) → 기준선 → 진행 방향 순
  auto dir_key = [&](int i) {
    const auto& o = objs[static_cast<std::size_t>(i)];
    return static_cast<int>(std::lround(std::atan2(o.dir_y, o.dir_x) * 180.0 / 3.14159265358979));
  };
  std::sort(idx.begin(), idx.end(), [&](int x, int y) {
    const int dx = dir_key(x), dy = dir_key(y);
    if (dx != dy) return dx < dy;
    const double nx = normal_of(objs[static_cast<std::size_t>(x)]), ny = normal_of(objs[static_cast<std::size_t>(y)]);
    if (std::abs(nx - ny) > 1e-6) return nx > ny;
    return along_range(objs[static_cast<std::size_t>(x)]).first < along_range(objs[static_cast<std::size_t>(y)]).first;
  });
  // 1) 같은 방향·기준선(±0.25em) 묶음
  std::vector<std::vector<int>> bands;
  for (int i : idx) {
    const auto& o = objs[static_cast<std::size_t>(i)];
    if (!bands.empty()) {
      const auto& last = objs[static_cast<std::size_t>(bands.back().front())];
      if (dir_key(bands.back().front()) == dir_key(i) &&
          std::abs(normal_of(last) - normal_of(o)) <= 0.25 * std::max(last.eff_size, o.eff_size)) {
        bands.back().push_back(i);
        continue;
      }
    }
    bands.push_back({i});
  }
  // 2) 묶음 안에서 진행 방향 순으로 정렬하고 1.5em 넘는 간격(단 구분)으로 자른다
  std::vector<Line> lines;
  for (auto& band : bands) {
    std::sort(band.begin(), band.end(), [&](int x, int y) {
      return along_range(objs[static_cast<std::size_t>(x)]).first < along_range(objs[static_cast<std::size_t>(y)]).first;
    });
    Line cur;
    double cur_end = -1e18;
    for (int i : band) {
      const auto& o = objs[static_cast<std::size_t>(i)];
      const auto [lo, hi] = along_range(o);
      if (!cur.members.empty() && lo - cur_end > 1.5 * std::max(cur.eff_size, o.eff_size)) {
        lines.push_back(std::move(cur));
        cur = Line{};
        cur_end = -1e18;
      }
      if (cur.members.empty()) {
        cur.baseline = normal_of(o);
        cur.dir_x = o.dir_x;
        cur.dir_y = o.dir_y;
        cur.along0 = lo;
      }
      cur.members.push_back(i);
      cur.eff_size = std::max(cur.eff_size, o.eff_size);
      cur.bounds.add(o.bounds);
      cur.fonts.insert(o.font);
      cur.sizes.insert(o.size);
      cur_end = std::max(cur_end, hi);
      cur.along1 = cur_end;
    }
    if (!cur.members.empty()) lines.push_back(std::move(cur));
  }
  return lines;
}

// ---------- 추출 줄 ----------

struct Extraction {
  std::map<FPDF_PAGEOBJECT, std::set<int>> obj_lines;  // 객체 → 추출 줄 id 들
  std::map<int, std::set<FPDF_PAGEOBJECT>> line_objs;
  std::vector<char32_t> chars;
  std::vector<FPDF_PAGEOBJECT> char_obj;
  std::vector<bool> generated;
  int lines = 0;
};

Extraction extract(FPDF_TEXTPAGE tp) {
  Extraction ex;
  const int n = FPDFText_CountChars(tp);
  int line = 0;
  bool line_has = false;
  for (int i = 0; i < n; ++i) {
    const char32_t c = FPDFText_GetUnicode(tp, i);
    FPDF_PAGEOBJECT o = FPDFText_GetTextObject(tp, i);
    ex.chars.push_back(c);
    ex.char_obj.push_back(o);
    ex.generated.push_back(FPDFText_IsGenerated(tp, i) == 1);
    if (c == '\r' || c == '\n') {
      if (line_has) ++line;
      line_has = false;
      continue;
    }
    if (o && !is_space(c)) {
      ex.obj_lines[o].insert(line);
      ex.line_objs[line].insert(o);
      line_has = true;
    }
  }
  ex.lines = line + (line_has ? 1 : 0);
  return ex;
}

// 줄 객체에 속한 글자를 추출 순서대로 이어 붙인다(생성된 공백은 객체 사이에서만 넣는다)
std::u32string line_text(const Extraction& ex, const std::set<FPDF_PAGEOBJECT>& members) {
  std::u32string out;
  bool pending_space = false;
  for (std::size_t i = 0; i < ex.chars.size(); ++i) {
    const char32_t c = ex.chars[i];
    if (c == '\r' || c == '\n') {
      if (!out.empty()) pending_space = true;
      continue;
    }
    if (ex.char_obj[i] && members.count(ex.char_obj[i])) {
      if (pending_space && !out.empty() && !is_space(out.back()) && !is_space(c)) out += U' ';
      pending_space = false;
      out += c;
    } else if (!out.empty() && is_space(c)) {
      pending_space = true;
    }
  }
  return trim(out);
}

// ---------- 글꼴 ----------

std::string font_name(FPDF_FONT f) {
  if (!f) return "?";
  const std::size_t n = FPDFFont_GetBaseFontName(f, nullptr, 0);
  if (n == 0) return "?";
  std::string s(n, '\0');
  FPDFFont_GetBaseFontName(f, s.data(), n);
  if (!s.empty() && s.back() == '\0') s.pop_back();
  return s;
}

struct GlyphSig {
  int segs = -1;
  std::vector<float> pts;
  bool operator==(const GlyphSig& o) const { return segs == o.segs && pts == o.pts; }
};
GlyphSig glyph_sig(FPDF_FONT f, char32_t c) {
  GlyphSig s;
  FPDF_GLYPHPATH gp = FPDFFont_GetGlyphPath(f, static_cast<uint32_t>(c), 12.0f);
  if (!gp) return s;
  s.segs = FPDFGlyphPath_CountGlyphSegments(gp);
  for (int i = 0; i < std::min(s.segs, 6); ++i) {
    float x = 0, y = 0;
    if (FPDF_PATHSEGMENT seg = FPDFGlyphPath_GetGlyphPathSegment(gp, i); seg && FPDFPathSegment_GetPoint(seg, &x, &y)) {
      s.pts.push_back(x);
      s.pts.push_back(y);
    }
  }
  return s;
}

struct Coverage {
  int have = 0, total = 0;
  [[nodiscard]] double ratio() const { return total ? static_cast<double>(have) / total : 0; }
};
Coverage coverage(FPDF_FONT f, const std::u32string& set) {
  const GlyphSig notdef = glyph_sig(f, 0xE000);  // 사용자 정의 영역 — 없을 글자의 서명(.notdef)
  Coverage cv;
  for (char32_t c : set) {
    ++cv.total;
    const GlyphSig s = glyph_sig(f, c);
    if (s.segs > 0 && !(notdef.segs > 0 && s == notdef)) ++cv.have;
  }
  return cv;
}

const std::u32string kDigits = U"0123456789";
const std::u32string kLower = U"abcdefghijklmnopqrstuvwxyz";
const std::u32string kUpper = U"ABCDEFGHIJKLMNOPQRSTUVWXYZ";
// 한글 상용 음절 100자 (빈도 상위권)
const std::u32string kHangul =
    U"이다는의에가을를하고한으로서도지기있수사대자정시것와리그적인어나일들해제에서전보까"
    U"요부상니게면문우동성주만원과이런경었다라중없며회국년같학계생라고말여합관분발화장업"
    U"내소용할공것들도세방위개무거";

// ---------- 결과 ----------

std::ofstream g_pages, g_lines, g_fonts;
std::set<std::pair<std::string, std::string>> g_fonts_seen;

struct Options {
  fs::path out;
  int pages = 2;
  double scale = 2.0;
  int lines_per_page = 3;
  fs::path cid_font;
};

std::shared_ptr<std::vector<std::uint8_t>> g_cid_font;

std::string safe_name(const fs::path& p) {
  std::string s = p.parent_path().filename().string() + "_" + p.stem().string();
  for (char& c : s)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
  return s;
}

std::string meta(FPDF_DOCUMENT doc, const char* key) {
  const unsigned long n = FPDF_GetMetaText(doc, key, nullptr, 0);
  if (n <= 2) return "";
  std::vector<unsigned short> buf(n / 2 + 1, 0);
  FPDF_GetMetaText(doc, key, buf.data(), n);
  return u8(utf16_to_u32(buf));
}

// 대체 글꼴: 한글이면 CID TrueType(전체 임베딩 — 스파이크 한정), 아니면 표준 14 글꼴을 글꼴 속성으로 고른다
FPDF_FONT substitute_font(FPDF_DOCUMENT doc, const TObj& o, const std::u32string& text, std::string& chosen) {
  if (has_hangul(text)) {
    if (!g_cid_font) return nullptr;
    chosen = "NotoSansKR(CID)";
    return FPDFText_LoadFont(doc, g_cid_font->data(), static_cast<uint32_t>(g_cid_font->size()), FPDF_FONT_TRUETYPE, 1);
  }
  const std::string name = font_name(o.font);
  std::string lname = name;
  std::transform(lname.begin(), lname.end(), lname.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const int flags = o.font ? FPDFFont_GetFlags(o.font) : 0;
  const int weight = o.font ? FPDFFont_GetWeight(o.font) : 400;
  int angle = 0;
  if (o.font) FPDFFont_GetItalicAngle(o.font, &angle);
  const bool fixed = (flags & 1) || lname.find("mono") != std::string::npos || lname.find("courier") != std::string::npos;
  const bool serif = (flags & 2) || lname.find("times") != std::string::npos || lname.find("serif") != std::string::npos ||
                     lname.find("cmr") != std::string::npos || lname.find("roman") != std::string::npos ||
                     lname.find("georgia") != std::string::npos || lname.find("cambria") != std::string::npos;
  const bool sans_hint = lname.find("sans") != std::string::npos || lname.find("arial") != std::string::npos ||
                         lname.find("helvet") != std::string::npos || lname.find("calibri") != std::string::npos;
  const bool bold = weight >= 600 || lname.find("bold") != std::string::npos || lname.find("cmbx") != std::string::npos ||
                    lname.find("black") != std::string::npos || lname.find("semibold") != std::string::npos;
  const bool italic = angle != 0 || (flags & 64) || lname.find("italic") != std::string::npos ||
                      lname.find("oblique") != std::string::npos || lname.find("cmti") != std::string::npos;
  std::string base;
  if (fixed) base = std::string("Courier") + (bold ? (italic ? "-BoldOblique" : "-Bold") : (italic ? "-Oblique" : ""));
  else if (serif && !sans_hint) base = bold ? (italic ? "Times-BoldItalic" : "Times-Bold") : (italic ? "Times-Italic" : "Times-Roman");
  else base = std::string("Helvetica") + (bold ? (italic ? "-BoldOblique" : "-Bold") : (italic ? "-Oblique" : ""));
  chosen = base;
  return FPDFText_LoadStandardFont(doc, base.c_str());
}

struct EditResult {
  bool ok = false;
  std::string error;
  DiffStats d;
  Rect new_bounds;
  double ms = 0;
  bool qpdf_ok = false;
  std::size_t qpdf_warnings = 0;
  std::size_t bytes = 0;
  std::u32string text_back;
  onpdf::Bitmap bmp;
};

// 편집된 문서를 저장 → 디스크 → qpdf check → 다시 열어 렌더
EditResult finish(FPDF_DOCUMENT doc, FPDF_PAGE page, int page_index, const Options& opt, const onpdf::Bitmap& base,
                  const std::optional<PixRect>& mask, const std::vector<int>& probe_path, std::chrono::steady_clock::time_point t0) {
  EditResult r;
  if (!FPDFPage_GenerateContent(page)) {
    r.error = "generate";
    return r;
  }
  auto saved = save_copy(doc);
  r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (!saved) {
    r.error = "save";
    return r;
  }
  r.bytes = saved->size();
  const fs::path tmp = opt.out / "tmp-edit.pdf";
  {
    std::ofstream o(tmp, std::ios::binary);
    o.write(reinterpret_cast<const char*>(saved->data()), static_cast<std::streamsize>(saved->size()));
  }
  if (auto c = onpdf::engine::QpdfTools::check(tmp)) {
    r.qpdf_ok = true;
    r.qpdf_warnings = c->warnings.size();
  }
  auto bytes = std::make_shared<std::vector<std::uint8_t>>(std::move(*saved));
  auto re = load(bytes);
  if (!re) {
    r.error = "reopen";
    return r;
  }
  PagePtr p2(FPDF_LoadPage(re->doc.get(), page_index));
  if (!p2) {
    r.error = "reload page";
    return r;
  }
  r.bmp = render(p2.get(), opt.scale);
  r.d = diff(base, r.bmp, mask);
  if (!probe_path.empty()) {
    FPDF_PAGEOBJECT parent = nullptr;
    if (FPDF_PAGEOBJECT o = resolve(p2.get(), probe_path, &parent); o && FPDFPageObj_GetType(o) == FPDF_PAGEOBJ_TEXT) {
      Mat container;
      if (parent) {
        // 단순화: Form 한 단계만 (경로 길이 2)
        container = get_matrix(parent);
      }
      r.new_bounds = bounds_in_page(o, container);
      TextPtr tp(FPDFText_LoadPage(p2.get()));
      if (tp) r.text_back = obj_text(o, tp.get());
    }
  }
  r.ok = true;
  return r;
}

void process_doc(const fs::path& file, const Options& opt) {
  auto bytes = std::make_shared<std::vector<std::uint8_t>>(read_file(file));
  auto ld = load(bytes);
  const std::string name = safe_name(file);
  if (!ld) {
    g_pages << name << "\t-\tLOAD_FAIL\n";
    return;
  }
  const std::string producer = meta(ld->doc.get(), "Producer");
  const int npages = FPDF_GetPageCount(ld->doc.get());
  const fs::path doc_out = opt.out / name;
  fs::create_directories(doc_out);

  for (int p = 0; p < std::min(npages, opt.pages); ++p) {
    PagePtr page(FPDF_LoadPage(ld->doc.get(), p));
    if (!page) continue;
    TextPtr tp(FPDFText_LoadPage(page.get()));
    Collector col;
    col.tp = tp.get();
    col.run(page.get());
    const onpdf::Bitmap base = render(page.get(), opt.scale);
    const Extraction ex = extract(tp.get());
    auto lines = group_lines(col.objs);

    // E1 지표
    int agree = 0, nontrivial = 0, nonhoriz = 0, mixed_font = 0, merged = 0;
    for (const auto& L : lines) {
      std::set<FPDF_PAGEOBJECT> mem;
      for (int i : L.members) mem.insert(col.objs[static_cast<std::size_t>(i)].handle);
      std::set<int> ids;
      for (auto* h : mem)
        if (auto it = ex.obj_lines.find(h); it != ex.obj_lines.end()) ids.insert(it->second.begin(), it->second.end());
      if (ids.empty()) continue;
      ++nontrivial;
      if (ids.size() > 1) ++merged;  // 서로 다른 추출 줄을 한 줄로 묶음 — 편집하면 다른 줄까지 바뀐다
      if (ids.size() == 1) {
        const auto& eo = ex.line_objs.at(*ids.begin());
        if (std::all_of(eo.begin(), eo.end(), [&](FPDF_PAGEOBJECT h) { return mem.count(h) > 0; })) ++agree;
      }
      if (std::abs(L.dir_y) > 0.01) ++nonhoriz;
      if (L.fonts.size() > 1 || L.sizes.size() > 1) ++mixed_font;
    }
    int multi_line_objs = 0, nested = 0;
    std::size_t total_chars = 0, text_objs = 0;
    for (const auto& o : col.objs) {
      if (auto it = ex.obj_lines.find(o.handle); it != ex.obj_lines.end() && it->second.size() > 1) ++multi_line_objs;
      if (o.depth > 0) ++nested;
      const auto t = trim(o.text);
      if (!t.empty()) {
        total_chars += t.size();
        ++text_objs;
      }
    }

    // E0 재생성 충실도: 텍스트 객체 하나를 항등 변환으로 더럽혀 그 스트림만 다시 쓴다
    std::string e0 = "-";
    long e0_px = -1;
    long e0_big = -1;
    if (!col.objs.empty()) {
      auto ld0 = load(bytes);
      PagePtr p0(FPDF_LoadPage(ld0->doc.get(), p));
      FPDF_PAGEOBJECT parent = nullptr;
      // 쪽 직속 텍스트 객체를 우선 (Form 안은 Form 스트림만 바뀐다)
      const TObj* pick = nullptr;
      for (const auto& o : col.objs)
        if (o.depth == 0) {
          pick = &o;
          break;
        }
      if (!pick) pick = &col.objs.front();
      if (FPDF_PAGEOBJECT o = resolve(p0.get(), pick->path, &parent)) {
        FPDFPageObj_Transform(o, 1, 0, 0, 1, 0, 0);
        const auto r = finish(ld0->doc.get(), p0.get(), p, opt, base, std::nullopt, {}, std::chrono::steady_clock::now());
        e0 = r.ok ? (r.qpdf_ok ? "ok" : "qpdf_fail") : r.error;
        e0_px = r.ok ? r.d.outside : -1;
        e0_big = r.ok ? r.d.outside_big : -1;
        if (r.ok && r.d.outside > 0) fs::copy_file(opt.out / "tmp-edit.pdf", doc_out / ("p" + std::to_string(p) + "-e0.pdf"), fs::copy_options::overwrite_existing);
        if (r.ok && r.d.outside > 0) write_diff_png(base, r.bmp, doc_out / ("p" + std::to_string(p) + "-e0-diff.png"));
      }
    }

    const double mean_chars = text_objs ? static_cast<double>(total_chars) / static_cast<double>(text_objs) : 0;
    g_pages << name << '\t' << p << '\t' << producer << '\t' << col.objs.size() << '\t' << nested << '\t' << col.forms << '\t'
            << col.shadings << '\t' << col.images << '\t' << col.paths << '\t' << fmt(mean_chars, 1) << '\t' << lines.size() << '\t'
            << ex.lines << '\t' << nontrivial << '\t' << agree << '\t'
            << fmt(nontrivial ? 100.0 * agree / nontrivial : 0, 1) << '\t' << merged << '\t' << multi_line_objs << '\t' << nonhoriz << '\t'
            << mixed_font << '\t' << e0 << '\t' << e0_px << '\t' << e0_big << '\n';
    g_pages.flush();

    // 대상 줄: 가로, 10자 이상, 쪽 위→아래 순서에서 25·50·75% 지점
    std::vector<int> cand;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
      const auto& L = lines[static_cast<std::size_t>(i)];
      if (std::abs(L.dir_y) > 0.01 || L.dir_x < 0) continue;
      std::set<FPDF_PAGEOBJECT> mem;
      for (int k : L.members) mem.insert(col.objs[static_cast<std::size_t>(k)].handle);
      if (line_text(ex, mem).size() >= 10) cand.push_back(i);
    }
    std::sort(cand.begin(), cand.end(), [&](int x, int y) { return lines[static_cast<std::size_t>(x)].bounds.t > lines[static_cast<std::size_t>(y)].bounds.t; });
    std::vector<int> targets;
    for (int k = 1; k <= opt.lines_per_page && !cand.empty(); ++k) {
      const int t = cand[std::min(cand.size() - 1, cand.size() * static_cast<std::size_t>(k) / static_cast<std::size_t>(opt.lines_per_page + 1))];
      if (std::find(targets.begin(), targets.end(), t) == targets.end()) targets.push_back(t);
    }

    for (std::size_t ti = 0; ti < targets.size(); ++ti) {
      const Line& L = lines[static_cast<std::size_t>(targets[ti])];
      std::set<FPDF_PAGEOBJECT> mem;
      std::vector<std::vector<int>> paths;
      for (int k : L.members) {
        mem.insert(col.objs[static_cast<std::size_t>(k)].handle);
        paths.push_back(col.objs[static_cast<std::size_t>(k)].path);
      }
      const std::u32string text = line_text(ex, mem);
      const TObj& first = col.objs[static_cast<std::size_t>(L.members.front())];
      // 줄 객체가 다른 줄의 글자도 품고 있으면(여러 줄 객체) 삭제가 다른 줄까지 지운다 → 기록
      bool spans_other = false;
      for (auto* h : mem)
        if (auto it = ex.obj_lines.find(h); it != ex.obj_lines.end() && it->second.size() > 1) spans_other = true;
      const int pad = std::max(3, static_cast<int>(std::lround(0.35 * L.eff_size * opt.scale)));
      const PixRect mask = to_device(page.get(), L.bounds, base, pad);

      auto remove_line = [&](FPDF_PAGE pg, bool keep_first) -> int {
        // 뒤 index 부터 지워야 앞 경로가 유효하다
        std::vector<std::vector<int>> order = paths;
        std::sort(order.begin(), order.end(), std::greater<>());
        int removed = 0;
        for (const auto& path : order) {
          if (keep_first && path == first.path) continue;
          FPDF_PAGEOBJECT parent = nullptr;
          FPDF_PAGEOBJECT o = resolve(pg, path, &parent);
          if (!o) continue;
          const bool ok = parent ? FPDFFormObj_RemoveObject(parent, o) : FPDFPage_RemoveObject(pg, o);
          if (ok) {
            FPDFPageObj_Destroy(o);
            ++removed;
          }
        }
        return removed;
      };

      // E2 삭제
      EditResult e2;
      {
        auto l2 = load(bytes);
        PagePtr p2(FPDF_LoadPage(l2->doc.get(), p));
        const auto t0 = std::chrono::steady_clock::now();
        remove_line(p2.get(), false);
        e2 = finish(l2->doc.get(), p2.get(), p, opt, base, mask, {}, t0);
        if (e2.ok && e2.d.outside > 0)
          write_diff_png(base, e2.bmp, doc_out / ("p" + std::to_string(p) + "-l" + std::to_string(ti) + "-e2-diff.png"));
      }
      // E3a 제자리 SetText (원본 글꼴)
      EditResult e3a;
      {
        auto l3 = load(bytes);
        PagePtr p3(FPDF_LoadPage(l3->doc.get(), p));
        const auto t0 = std::chrono::steady_clock::now();
        remove_line(p3.get(), true);
        FPDF_PAGEOBJECT parent = nullptr;
        // 앞 객체를 지우지 않았으므로 첫 객체 경로가 유효하다 — 단 같은 컨테이너의 뒤 객체만 지웠을 때. 첫 객체보다 앞 index 가
        // 지워졌을 수 있어 경로를 다시 찾는다: 멤버 중 첫 객체보다 앞 index 의 개수만큼 당긴다
        std::vector<int> fp = first.path;
        for (const auto& path : paths)
          if (path != first.path && path.size() == fp.size() &&
              std::equal(path.begin(), path.end() - 1, fp.begin()) && path.back() < first.path.back())
            --fp.back();
        FPDF_PAGEOBJECT o = resolve(p3.get(), fp, &parent);
        auto w = u32_to_utf16z(text);
        if (o && FPDFText_SetText(o, reinterpret_cast<FPDF_WIDESTRING>(w.data()))) {
          e3a = finish(l3->doc.get(), p3.get(), p, opt, base, mask, fp, t0);
        } else {
          e3a.error = "settext";
        }
      }
      // E3b 대체 글꼴 (새 객체, 쪽 직속)
      EditResult e3b;
      std::string sub_name = "-";
      {
        auto l4 = load(bytes);
        PagePtr p4(FPDF_LoadPage(l4->doc.get(), p));
        const auto t0 = std::chrono::steady_clock::now();
        remove_line(p4.get(), false);
        FPDF_FONT f = substitute_font(l4->doc.get(), first, text, sub_name);
        FPDF_PAGEOBJECT o = f ? FPDFPageObj_CreateTextObj(l4->doc.get(), f, first.size) : nullptr;
        auto w = u32_to_utf16z(text);
        if (o && FPDFText_SetText(o, reinterpret_cast<FPDF_WIDESTRING>(w.data()))) {
          FPDFPageObj_SetFillColor(o, first.r, first.g, first.b, first.a);
          FPDFPageObj_Transform(o, first.m.a, first.m.b, first.m.c, first.m.d, first.m.e, first.m.f);
          FPDFPage_InsertObject(p4.get(), o);
          const int idx = FPDFPage_CountObjects(p4.get()) - 1;
          e3b = finish(l4->doc.get(), p4.get(), p, opt, base, mask, {idx}, t0);
        } else {
          e3b.error = f ? "settext" : "font";
          if (o) FPDFPageObj_Destroy(o);
        }
        if (f) FPDFFont_Close(f);
      }

      // E5 수술: qpdf 로 줄의 표시 연산자만 지우고(나머지 바이트 그대로) 새 텍스트는 PDFium 이 덧붙인 스트림에 둔다
      std::set<int> page_ords;
      std::map<int, std::set<int>> form_ords;
      bool deep = false;
      int pdfium_page_texts = 0;
      for (const auto& o : col.objs)
        if (o.path.size() == 1) ++pdfium_page_texts;
      for (const auto& path : paths) {
        if (path.size() == 1) {
          int ord = 0;
          for (const auto& o : col.objs)
            if (o.path.size() == 1 && o.path[0] < path[0]) ++ord;
          page_ords.insert(ord);
        } else if (path.size() == 2) {
          int fo = 0;
          for (int k = 0; k < path[0]; ++k)
            if (FPDF_PAGEOBJECT x = FPDFPage_GetObject(page.get(), k); x && FPDFPageObj_GetType(x) == FPDF_PAGEOBJ_FORM) ++fo;
          int ord = 0;
          for (const auto& o : col.objs)
            if (o.path.size() == 2 && o.path[0] == path[0] && o.path[1] < path[1]) ++ord;
          form_ords[fo].insert(ord);
        } else {
          deep = true;
        }
      }
      EditResult e5d, e5n;
      spike::SurgeryResult s5;
      if (!deep) {
        // E5-del: 삭제만
        s5 = spike::remove_show_ops(*bytes, p, page_ords, form_ords);
        if (s5.ok) {
          auto l5 = load(std::make_shared<std::vector<std::uint8_t>>(s5.bytes));
          if (l5) {
            PagePtr p5(FPDF_LoadPage(l5->doc.get(), p));
            e5d.bmp = render(p5.get(), opt.scale);
            e5d.d = diff(base, e5d.bmp, mask);
            e5d.ok = true;
          } else {
            e5d.error = "reopen";
          }
        } else {
          e5d.error = s5.error;
        }
        // E5-new: 원본 글꼴로 새 객체를 덧붙인 뒤(원본 스트림은 더럽히지 않음) 수술
        auto l6 = load(bytes);
        PagePtr p6(FPDF_LoadPage(l6->doc.get(), p));
        FPDF_PAGEOBJECT src = nullptr;
        {
          FPDF_PAGEOBJECT parent = nullptr;
          src = resolve(p6.get(), first.path, &parent);
        }
        FPDF_FONT f = src ? FPDFTextObj_GetFont(src) : nullptr;
        FPDF_PAGEOBJECT o = f ? FPDFPageObj_CreateTextObj(l6->doc.get(), f, first.size) : nullptr;
        auto w = u32_to_utf16z(text);
        if (o && FPDFText_SetText(o, reinterpret_cast<FPDF_WIDESTRING>(w.data()))) {
          FPDFPageObj_SetFillColor(o, first.r, first.g, first.b, first.a);
          FPDFPageObj_Transform(o, first.m.a, first.m.b, first.m.c, first.m.d, first.m.e, first.m.f);
          FPDFPage_InsertObject(p6.get(), o);
          if (FPDFPage_GenerateContent(p6.get())) {
            if (auto saved = save_copy(l6->doc.get())) {
              auto s6 = spike::remove_show_ops(*saved, p, page_ords, form_ords);
              if (s6.ok) {
                auto l7 = load(std::make_shared<std::vector<std::uint8_t>>(s6.bytes));
                PagePtr p7(l7 ? FPDF_LoadPage(l7->doc.get(), p) : nullptr);
                if (p7) {
                  e5n.bmp = render(p7.get(), opt.scale);
                  e5n.d = diff(base, e5n.bmp, mask);
                  const int last = FPDFPage_CountObjects(p7.get()) - 1;
                  if (FPDF_PAGEOBJECT no = FPDFPage_GetObject(p7.get(), last); no && FPDFPageObj_GetType(no) == FPDF_PAGEOBJ_TEXT) {
                    e5n.new_bounds = bounds_in_page(no, Mat{});
                    TextPtr tp7(FPDFText_LoadPage(p7.get()));
                    if (tp7) e5n.text_back = obj_text(no, tp7.get());
                  }
                  e5n.ok = true;
                  const fs::path tmp = opt.out / "tmp-e5.pdf";
                  {
                    std::ofstream of(tmp, std::ios::binary);
                    of.write(reinterpret_cast<const char*>(s6.bytes.data()), static_cast<std::streamsize>(s6.bytes.size()));
                  }
                  e5n.qpdf_ok = static_cast<bool>(onpdf::engine::QpdfTools::check(tmp));
                }
              } else {
                e5n.error = s6.error;
              }
            }
          }
        } else {
          e5n.error = f ? "settext" : "font";
          if (o) FPDFPageObj_Destroy(o);
        }
      } else {
        e5d.error = e5n.error = "deep-form";
      }
      if (e5d.ok && e5d.d.outside > 0)
        write_diff_png(base, e5d.bmp, doc_out / ("p" + std::to_string(p) + "-l" + std::to_string(ti) + "-e5-diff.png"));

      // 비교 이미지: 원본 / 제자리 / 대체
      const int margin = static_cast<int>(std::lround(0.6 * L.eff_size * opt.scale));
      PixRect cr{std::max(0, mask.x0 - margin), std::max(0, mask.y0 - margin), std::min(base.width, mask.x1 + margin),
                 std::min(base.height, mask.y1 + margin)};
      std::vector<onpdf::Bitmap> parts{crop(base, cr)};
      if (e3a.ok) parts.push_back(crop(e3a.bmp, cr));
      if (e3b.ok) parts.push_back(crop(e3b.bmp, cr));
      if (e5n.ok) parts.push_back(crop(e5n.bmp, cr));
      (void)onpdf::image::write_png(stack(parts), doc_out / ("p" + std::to_string(p) + "-l" + std::to_string(ti) + ".png"));

      const double orig_w = L.bounds.r - L.bounds.l;
      auto wr = [&](const EditResult& r) { return r.ok && r.new_bounds.valid() && orig_w > 0 ? (r.new_bounds.r - r.new_bounds.l) / orig_w : 0.0; };
      auto dl = [&](const EditResult& r) { return r.ok && r.new_bounds.valid() ? r.new_bounds.l - L.bounds.l : 0.0; };
      auto inside_ratio = [&](const EditResult& r) { return r.ok && r.d.mask_area ? 100.0 * static_cast<double>(r.d.inside) / static_cast<double>(r.d.mask_area) : -1.0; };
      std::set<std::string> fnames;
      for (auto* f : L.fonts) fnames.insert(font_name(f) + (f && FPDFFont_GetIsEmbedded(f) == 1 ? "" : "(ne)"));
      std::string fjoin;
      for (const auto& n : fnames) fjoin += (fjoin.empty() ? "" : ",") + n;

      g_lines << name << '\t' << p << '\t' << ti << '\t' << L.members.size() << '\t' << (spans_other ? 1 : 0) << '\t'
              << (first.depth > 0 ? 1 : 0) << '\t' << fmt(L.eff_size, 1) << '\t' << fjoin << '\t'
              // E2
              << (e2.ok ? (e2.qpdf_ok ? "ok" : "qpdf_fail") : e2.error) << '\t' << e2.d.outside << '\t' << e2.d.outside_big << '\t'
              << fmt(e2.ms, 1) << '\t'
              // E3a
              << (e3a.ok ? "ok" : e3a.error) << '\t' << (e3a.ok ? e3a.d.outside : -1) << '\t' << fmt(wr(e3a), 3) << '\t'
              << fmt(dl(e3a), 2) << '\t' << fmt(inside_ratio(e3a), 1) << '\t'
              << (e3a.ok ? (trim(e3a.text_back) == text ? "same" : "diff") : "-") << '\t'
              // E3b
              << sub_name << '\t' << (e3b.ok ? "ok" : e3b.error) << '\t' << (e3b.ok ? e3b.d.outside : -1) << '\t' << fmt(wr(e3b), 3)
              << '\t' << fmt(dl(e3b), 2) << '\t' << fmt(inside_ratio(e3b), 1) << '\t' << (e5d.ok ? "ok" : e5d.error) << '\t' << (e5d.ok ? e5d.d.outside : -1) << '\t' << (e5d.ok ? e5d.d.outside_big : -1)
              << '\t' << pdfium_page_texts << '\t' << s5.page_show_ops << '\t' << s5.needs_displacement << '\t'
              << (e5n.ok ? (e5n.qpdf_ok ? "ok" : "qpdf_fail") : e5n.error) << '\t' << (e5n.ok ? e5n.d.outside_big : -1) << '\t'
              << fmt(wr(e5n), 3) << '\t' << fmt(dl(e5n), 2) << '\t' << (e5n.ok ? (trim(e5n.text_back) == text ? "same" : "diff") : "-")
              << '\t' << u8(text).substr(0, 60) << '\n';
      g_lines.flush();

      // E4 글꼴 커버리지 (줄 글꼴별 한 번)
      for (auto* f : L.fonts) {
        if (!f) continue;
        const std::string fname = font_name(f);
        if (!g_fonts_seen.insert({name, fname}).second) continue;
        const auto cd = coverage(f, kDigits), cl = coverage(f, kLower), cu = coverage(f, kUpper), ch = coverage(f, kHangul);
        std::size_t data_len = 0;
        FPDFFont_GetFontData(f, nullptr, 0, &data_len);
        g_fonts << name << '\t' << fname << '\t' << FPDFFont_GetIsEmbedded(f) << '\t' << (fname.find('+') == 6 ? 1 : 0) << '\t'
                << data_len << '\t' << fmt(100 * cd.ratio(), 0) << '\t' << fmt(100 * cl.ratio(), 0) << '\t'
                << fmt(100 * cu.ratio(), 0) << '\t' << fmt(100 * ch.ratio(), 0) << '\n';
        g_fonts.flush();
      }
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  std::vector<fs::path> files;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--pages" && i + 1 < argc) opt.pages = std::atoi(argv[++i]);
    else if (a == "--scale" && i + 1 < argc) opt.scale = std::atof(argv[++i]);
    else if (a == "--lines" && i + 1 < argc) opt.lines_per_page = std::atoi(argv[++i]);
    else if (a == "--font" && i + 1 < argc) opt.cid_font = onpdf::path_from_utf8(argv[++i]);
    else if (opt.out.empty()) opt.out = onpdf::path_from_utf8(a);
    else files.push_back(onpdf::path_from_utf8(a));
  }
  if (opt.out.empty() || files.empty()) {
    std::cerr << "usage: onpdf_spike_text_edit <out-dir> [--pages N] [--scale S] [--lines K] [--font cid.ttf] a.pdf ...\n";
    return 2;
  }
  fs::create_directories(opt.out);
  if (!opt.cid_font.empty()) g_cid_font = std::make_shared<std::vector<std::uint8_t>>(read_file(opt.cid_font));

  FPDF_LIBRARY_CONFIG cfg{};
  cfg.version = 2;
  FPDF_InitLibraryWithConfig(&cfg);

  g_pages.open(opt.out / "pages.tsv");
  g_lines.open(opt.out / "lines.tsv");
  g_fonts.open(opt.out / "fonts.tsv");
  g_pages << "doc\tpage\tproducer\ttext_objs\tnested\tforms\tshadings\timages\tpaths\tchars_per_obj\tlines\textract_lines\t"
             "matched_lines\tagree\tagree_pct\tmerged\tmulti_line_objs\tnonhoriz_lines\tmixed_font_lines\te0\te0_diff_px\te0_big\n";
  g_lines << "doc\tpage\tline\tobjs\tspans_other\tin_form\tsize\tfonts\te2\te2_out_px\te2_out_big\te2_ms\te3a\te3a_out_px\t"
             "e3a_width_ratio\te3a_dx\te3a_inside_pct\te3a_text\tsub_font\te3b\te3b_out_px\te3b_width_ratio\te3b_dx\te3b_inside_pct\te5d\te5d_out_px\te5d_out_big\tpdfium_texts\tqpdf_show_ops\tneeds_disp\te5n\te5n_out_big\te5n_width_ratio\te5n_dx\te5n_text\ttext\n";
  g_fonts << "doc\tfont\tembedded\tsubset_tag\tdata_bytes\tdigits\tlower\tupper\thangul\n";

  for (const auto& f : files) {
    std::cerr << "[spike] " << f.string() << "\n";
    process_doc(f, opt);
  }
  FPDF_DestroyLibrary();
  return 0;
}
