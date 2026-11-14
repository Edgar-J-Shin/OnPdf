#include "onpdf/core/engine/text_page.h"

#include "onpdf/core/engine/pdfium_library.h"

#include <fpdf_doc.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace onpdf::engine {

namespace {

constexpr char32_t kReplacement = 0xFFFD;

struct PageSpace {
  FPDF_PAGE page;
  int w1000;
  int h1000;

  static PageSpace of(FPDF_PAGE page) {
    return {page, static_cast<int>(std::lround(FPDF_GetPageWidthF(page) * 1000.0)),
            static_cast<int>(std::lround(FPDF_GetPageHeightF(page) * 1000.0))};
  }

  Point to_pt(double x, double y) const {
    int dx = 0, dy = 0;
    if (!FPDF_PageToDevice(page, 0, 0, w1000, h1000, 0, x, y, &dx, &dy)) return {0.0, 0.0};
    return {dx / 1000.0, dy / 1000.0};
  }

  // 사용자 좌표 사각형 → 쪽 포인트 bbox (회전되면 꼭짓점이 뒤바뀌므로 4점 변환)
  Rect to_rect(double left, double right, double bottom, double top) const {
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
};

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

std::string uri_of(FPDF_DOCUMENT doc, FPDF_ACTION action) {
  const unsigned long len = FPDFAction_GetURIPath(doc, action, nullptr, 0);  // NUL 포함 길이
  if (len <= 1) return {};
  std::string buf(len, '\0');
  FPDFAction_GetURIPath(doc, action, buf.data(), len);
  buf.resize(len - 1);
  return buf;
}

}  // namespace

Result<TextPage> TextPage::load(FPDF_PAGE page) {
  PdfiumLibrary::assert_current_thread();
  ScopedTextPage text_page(FPDFText_LoadPage(page));
  if (!text_page) return make_error(ErrorCode::kRenderFailed, "FPDFText_LoadPage failed");
  return TextPage(std::move(text_page));
}

Result<view::PageTextIndex> TextPage::extract(FPDF_DOCUMENT doc, FPDF_PAGE page, int page_index) const {
  PdfiumLibrary::assert_current_thread();
  view::PageTextIndex index;
  index.page = page_index;
  const PageSpace space = PageSpace::of(page);
  FPDF_TEXTPAGE tp = text_page_.get();

  const int count = FPDFText_CountChars(tp);
  if (count < 0) return make_error(ErrorCode::kRenderFailed, "FPDFText_CountChars failed");
  index.chars.reserve(static_cast<std::size_t>(count));

  // 줄 묶음 상태. 기준선·x 진행 비교는 **회전 전 사용자 좌표**로 한다 — /Rotate 90 쪽도 사용자 좌표에서는
  // 글이 가로로 흐르므로, 변환된 좌표로 비교하면 글자마다 줄이 끊긴다
  int line_start = 0;
  double line_baseline = 0.0;  // 사용자 좌표 origin y
  float line_font = 0.0f;
  double prev_right = 0.0;     // 사용자 좌표 right
  bool line_open = false;
  Rect line_box;
  bool line_has_box = false;

  auto close_line = [&] {
    const int end = index.char_count();
    if (line_open && end > line_start) {
      index.lines.push_back({{line_start, end}, line_has_box ? line_box : Rect{}});
    }
    line_open = false;
    line_has_box = false;
    line_start = end;
  };

  for (int i = 0; i < count; ++i) {
    unsigned int code = FPDFText_GetUnicode(tp, i);
    if (code == U'\r' || code == U'\n') {  // PDFium 이 넣은 줄바꿈(생성 여부와 무관하게 줄 경계)
      close_line();
      continue;
    }
    view::TextChar ch;
    ch.generated = FPDFText_IsGenerated(tp, i) == 1;
    if (code == 0 || FPDFText_HasUnicodeMapError(tp, i) == 1) code = kReplacement;
    ch.code = static_cast<char32_t>(code);
    ch.font_size = static_cast<float>(FPDFText_GetFontSize(tp, i));
    double l = 0, r = 0, b = 0, t = 0;
    bool has_box = false;
    if (!ch.generated && FPDFText_GetCharBox(tp, i, &l, &r, &b, &t)) {
      ch.box = space.to_rect(l, r, b, t);
      if (ch.box.w <= 0.0 || ch.box.h <= 0.0) ch.box = {};
      has_box = !ch.box.empty();
    }
    double ox = 0, oy = 0;
    const bool has_origin = FPDFText_GetCharOrigin(tp, i, &ox, &oy) != 0;
    if (has_origin) ch.baseline_y = space.to_pt(ox, oy).y;

    // 기하 규칙(사용자 좌표): 기준선이 크게 다르거나 x 가 뒤로 가면 새 줄 (PDFium 줄바꿈이 없는 문서 대비)
    if (line_open && has_box && has_origin) {
      const double fs = std::max(ch.font_size, line_font);
      if (std::abs(oy - line_baseline) > 0.5 * fs || l < prev_right - fs) close_line();
    }
    if (!line_open) {
      line_open = true;
      line_start = index.char_count();
      line_baseline = oy;
      line_font = ch.font_size;
    }
    if (!ch.box.empty()) {
      if (!line_has_box) {
        line_box = ch.box;
        line_has_box = true;
      } else {
        const double x0 = std::min(line_box.x, ch.box.x), y0 = std::min(line_box.y, ch.box.y);
        const double x1 = std::max(line_box.right(), ch.box.right()), y1 = std::max(line_box.bottom(), ch.box.bottom());
        line_box = {x0, y0, x1 - x0, y1 - y0};
      }
      prev_right = r;
    }
    index.chars.push_back(ch);
  }
  close_line();

  // 링크 주석
  int pos = 0;
  FPDF_LINK link_handle = nullptr;
  while (FPDFLink_Enumerate(page, &pos, &link_handle)) {
    if (link_handle == nullptr) continue;
    FS_RECTF rect{};
    if (!FPDFLink_GetAnnotRect(link_handle, &rect)) continue;
    view::Link link;
    link.box = space.to_rect(rect.left, rect.right, rect.bottom, rect.top);
    if (FPDF_DEST dest = FPDFLink_GetDest(doc, link_handle); dest != nullptr) {
      link.kind = view::LinkKind::kPage;
      if (!fill_location(doc, dest, link)) continue;
    } else if (FPDF_ACTION action = FPDFLink_GetAction(link_handle); action != nullptr) {
      const unsigned long type = FPDFAction_GetType(action);
      if (type == PDFACTION_URI) {
        link.kind = view::LinkKind::kUri;
        link.uri = uri_of(doc, action);
        if (link.uri.empty()) continue;
      } else if (type == PDFACTION_GOTO) {
        FPDF_DEST action_dest = FPDFAction_GetDest(doc, action);
        if (action_dest == nullptr) continue;
        link.kind = view::LinkKind::kPage;
        if (!fill_location(doc, action_dest, link)) continue;
      } else {
        continue;  // REMOTEGOTO, LAUNCH, EMBEDDEDGOTO 는 지원하지 않음 (보안)
      }
    } else {
      continue;
    }
    index.links.push_back(std::move(link));
  }
  return index;
}

}  // namespace onpdf::engine
