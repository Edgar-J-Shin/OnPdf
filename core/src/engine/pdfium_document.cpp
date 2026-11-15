#include "onpdf/core/engine/pdfium_document.h"

#include "onpdf/core/engine/pdfium_library.h"
#include "dest.h"
#include "onpdf/core/engine/text_page.h"
#include "onpdf/core/platform/path.h"

#include <fpdf_doc.h>
#include <fpdfview.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_set>

namespace onpdf::engine {

namespace {

Error last_pdfium_error(const std::string& context) {
  const unsigned long code = FPDF_GetLastError();
  ErrorCode mapped = ErrorCode::kUnknown;
  switch (code) {
    case FPDF_ERR_FILE: mapped = ErrorCode::kFileNotFound; break;
    case FPDF_ERR_FORMAT: mapped = ErrorCode::kInvalidFormat; break;
    case FPDF_ERR_PASSWORD: mapped = ErrorCode::kPasswordRequired; break;
    case FPDF_ERR_SECURITY: mapped = ErrorCode::kSecurityError; break;
    case FPDF_ERR_PAGE: mapped = ErrorCode::kPageNotFound; break;
    default: break;
  }
  return Error{mapped, context + " (FPDF_GetLastError=" + std::to_string(code) + ")"};
}

Result<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    return make_error(ErrorCode::kFileNotFound, "file not found: " + path_to_utf8(path));
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return make_error(ErrorCode::kIoError, "cannot open: " + path_to_utf8(path));
  }
  std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
  if (in.bad()) {
    return make_error(ErrorCode::kIoError, "read failed: " + path_to_utf8(path));
  }
  return data;
}

}  // namespace

PdfiumDocument::PdfiumDocument(std::vector<std::uint8_t> data, ScopedDocument doc)
    : data_(std::move(data)), doc_(std::move(doc)) {}

Result<PdfiumDocument> PdfiumDocument::open(const std::filesystem::path& path,
                                            std::string_view password) {
  auto data = read_file(path);
  if (!data) {
    return std::unexpected(data.error());
  }
  return open_from_memory(std::move(*data), password);
}

Result<PdfiumDocument> PdfiumDocument::open_from_memory(std::vector<std::uint8_t> data,
                                                        std::string_view password) {
  PdfiumLibrary::assert_current_thread();
  if (data.empty()) {
    return make_error(ErrorCode::kInvalidFormat, "empty document");
  }
  const std::string pw(password);
  ScopedDocument doc(FPDF_LoadMemDocument64(data.data(), data.size(),
                                            pw.empty() ? nullptr : pw.c_str()));
  if (!doc) {
    return std::unexpected(last_pdfium_error("FPDF_LoadMemDocument64 failed"));
  }
  return PdfiumDocument(std::move(data), std::move(doc));
}

int PdfiumDocument::page_count() const noexcept {
  PdfiumLibrary::assert_current_thread();
  return FPDF_GetPageCount(doc_.get());
}

Result<ScopedPage> PdfiumDocument::load_page(int page_index) const {
  if (page_index < 0 || page_index >= page_count()) {
    return make_error(ErrorCode::kPageNotFound,
                      "page index out of range: " + std::to_string(page_index));
  }
  ScopedPage page(FPDF_LoadPage(doc_.get(), page_index));
  if (!page) {
    return std::unexpected(last_pdfium_error("FPDF_LoadPage failed"));
  }
  return page;
}

Result<PageSize> PdfiumDocument::page_size(int page_index) const {
  PdfiumLibrary::assert_current_thread();
  FS_SIZEF size{};
  if (page_index < 0 || page_index >= page_count() ||
      !FPDF_GetPageSizeByIndexF(doc_.get(), page_index, &size)) {
    return make_error(ErrorCode::kPageNotFound,
                      "page index out of range: " + std::to_string(page_index));
  }
  return PageSize{size.width, size.height};
}

Result<Bitmap> PdfiumDocument::render_page(int page_index, const RenderOptions& options) const {
  PdfiumLibrary::assert_current_thread();
  if (!(options.scale > 0.0f) || !std::isfinite(options.scale)) {
    return make_error(ErrorCode::kInvalidArgument, "scale must be positive");
  }
  int rotate = 0;
  switch (options.rotation) {
    case 0: rotate = 0; break;
    case 90: rotate = 1; break;
    case 180: rotate = 2; break;
    case 270: rotate = 3; break;
    default: return make_error(ErrorCode::kInvalidArgument, "rotation must be 0/90/180/270");
  }

  auto page = load_page(page_index);
  if (!page) {
    return std::unexpected(page.error());
  }

  double page_w = FPDF_GetPageWidthF(page->get());
  double page_h = FPDF_GetPageHeightF(page->get());
  if (rotate % 2 == 1) {
    std::swap(page_w, page_h);
  }
  const int width = std::max(1, static_cast<int>(std::lround(page_w * options.scale)));
  const int height = std::max(1, static_cast<int>(std::lround(page_h * options.scale)));

  ScopedBitmap bitmap(FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRA, nullptr, 0));
  if (!bitmap) {
    return make_error(ErrorCode::kRenderFailed, "FPDFBitmap_CreateEx failed");
  }
  FPDFBitmap_FillRect(bitmap.get(), 0, 0, width, height, 0xFFFFFFFF);

  int flags = 0;
  if (options.render_annotations) flags |= FPDF_ANNOT;
  if (options.lcd_text) flags |= FPDF_LCD_TEXT;
  if (options.printing) flags |= FPDF_PRINTING;
  if (options.grayscale) flags |= FPDF_GRAYSCALE;
  FPDF_RenderPageBitmap(bitmap.get(), page->get(), 0, 0, width, height, rotate, flags);

  const int stride = FPDFBitmap_GetStride(bitmap.get());
  const auto* buffer = static_cast<const std::uint8_t*>(FPDFBitmap_GetBuffer(bitmap.get()));
  if (buffer == nullptr || stride <= 0) {
    return make_error(ErrorCode::kRenderFailed, "FPDFBitmap_GetBuffer failed");
  }

  Bitmap out;
  out.width = width;
  out.height = height;
  out.stride = width * 4;
  out.pixels.resize(static_cast<std::size_t>(out.stride) * static_cast<std::size_t>(height));
  for (int y = 0; y < height; ++y) {
    std::memcpy(out.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(out.stride),
                buffer + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride),
                static_cast<std::size_t>(out.stride));
  }
  return out;
}

Result<Bitmap> PdfiumDocument::render_region(int page_index, const RenderOptions& options,
                                             const Rect& region_px) const {
  PdfiumLibrary::assert_current_thread();
  if (!(options.scale > 0.0f) || !std::isfinite(options.scale)) {
    return make_error(ErrorCode::kInvalidArgument, "scale must be positive");
  }
  const int width = static_cast<int>(std::ceil(region_px.w));
  const int height = static_cast<int>(std::ceil(region_px.h));
  if (width <= 0 || height <= 0) {
    return make_error(ErrorCode::kInvalidArgument, "region must be non-empty");
  }
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());

  ScopedBitmap bitmap(FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRA, nullptr, 0));
  if (!bitmap) return make_error(ErrorCode::kRenderFailed, "FPDFBitmap_CreateEx failed");
  FPDFBitmap_FillRect(bitmap.get(), 0, 0, width, height, 0xFFFFFFFF);

  // 매트릭스는 "회전 0, 쪽 크기 그대로 렌더했을 때의 장치 좌표(좌상단 원점)" 에 적용된다.
  // 배율 s 로 키우고 영역 원점이 (0,0) 에 오도록 평행이동.
  const FS_MATRIX matrix{options.scale, 0.0f, 0.0f, options.scale,
                         static_cast<float>(-region_px.x), static_cast<float>(-region_px.y)};
  const FS_RECTF clip{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)};
  int flags = 0;
  if (options.render_annotations) flags |= FPDF_ANNOT;
  if (options.lcd_text) flags |= FPDF_LCD_TEXT;
  if (options.printing) flags |= FPDF_PRINTING;
  if (options.grayscale) flags |= FPDF_GRAYSCALE;
  FPDF_RenderPageBitmapWithMatrix(bitmap.get(), page->get(), &matrix, &clip, flags);

  const int stride = FPDFBitmap_GetStride(bitmap.get());
  const auto* buffer = static_cast<const std::uint8_t*>(FPDFBitmap_GetBuffer(bitmap.get()));
  if (buffer == nullptr || stride <= 0) {
    return make_error(ErrorCode::kRenderFailed, "FPDFBitmap_GetBuffer failed");
  }
  Bitmap out;
  out.width = width;
  out.height = height;
  out.stride = width * 4;
  out.pixels.resize(static_cast<std::size_t>(out.stride) * static_cast<std::size_t>(height));
  for (int y = 0; y < height; ++y) {
    std::memcpy(out.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(out.stride),
                buffer + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride),
                static_cast<std::size_t>(out.stride));
  }
  return out;
}

Result<view::PageTextIndex> PdfiumDocument::load_text(int page_index) const {
  PdfiumLibrary::assert_current_thread();
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  auto text_page = TextPage::load(page->get());
  if (!text_page) return std::unexpected(text_page.error());
  return text_page->extract(doc_.get(), page->get(), page_index);
}

Result<view::Outline> PdfiumDocument::load_outline() const {
  PdfiumLibrary::assert_current_thread();
  view::Outline outline;
  FPDF_DOCUMENT doc = doc_.get();
  const int page_count = FPDF_GetPageCount(doc);

  // 명시적 스택 전위 순회: 프레임은 "아직 처리하지 않은 형제 체인의 다음 노드".
  // 자식 체인 프레임을 위에 쌓아 자식을 형제보다 먼저 처리한다 (재귀 없음 → C++ 스택 깊이와 무관)
  struct Frame {
    FPDF_BOOKMARK next;
    int parent;
    int depth;
  };
  std::vector<Frame> stack;
  std::unordered_set<FPDF_BOOKMARK> visited;
  stack.push_back({FPDFBookmark_GetFirstChild(doc, nullptr), -1, 0});

  while (!stack.empty()) {
    const std::size_t top = stack.size() - 1;
    FPDF_BOOKMARK node = stack[top].next;
    if (node == nullptr) {
      stack.pop_back();
      continue;
    }
    if (!visited.insert(node).second) {
      // 이미 본 북마크: 형제·자식 순환. 이 체인을 끊는다
      outline.cycle_detected = true;
      stack.pop_back();
      continue;
    }
    if (outline.size() >= view::Outline::kMaxItems) {
      outline.truncated = true;
      break;
    }
    const int parent = stack[top].parent;
    const int depth = stack[top].depth;
    stack[top].next = FPDFBookmark_GetNextSibling(doc, node);

    view::OutlineItem item;
    item.depth = depth;
    item.parent = parent;
    item.open = FPDFBookmark_GetCount(node) > 0;
    // 제목: UTF-16LE, 바이트 수(NUL 포함) 반환
    const unsigned long title_bytes = FPDFBookmark_GetTitle(node, nullptr, 0);
    if (title_bytes > 2) {
      std::vector<unsigned char> buf(title_bytes);
      FPDFBookmark_GetTitle(node, buf.data(), title_bytes);
      item.title = detail::utf16le_to_utf8(buf.data(), buf.size());
    }
    // 대상: /Dest 우선, 없으면 액션(GoTo/URI). Launch·RemoteGoTo 는 무시 (보안)
    view::Link link;
    bool has_target = false;
    if (FPDF_DEST dest = FPDFBookmark_GetDest(doc, node); dest != nullptr) {
      has_target = detail::fill_target_lazy(doc, dest, link);
    } else if (FPDF_ACTION action = FPDFBookmark_GetAction(node); action != nullptr) {
      const unsigned long type = FPDFAction_GetType(action);
      if (type == PDFACTION_GOTO) {
        if (FPDF_DEST d = FPDFAction_GetDest(doc, action); d != nullptr) has_target = detail::fill_target_lazy(doc, d, link);
      } else if (type == PDFACTION_URI) {
        link.kind = view::LinkKind::kUri;
        link.uri = detail::uri_of(doc, action);
        has_target = !link.uri.empty();
      }
    }
    if (has_target) {
      if (link.kind == view::LinkKind::kPage && link.user_location && link.page < page_count) {
        // 근사 쪽 포인트 y: 쪽을 열지 않고 높이로 뒤집기 (회전 없는 쪽에서 정확)
        FS_SIZEF size{};
        if (FPDF_GetPageSizeByIndexF(doc, link.page, &size)) {
          item.approx_y = std::clamp(static_cast<double>(size.height) - link.user_location->y, 0.0,
                                     static_cast<double>(size.height));
        }
      }
      item.target = std::move(link);
    }

    const int index = outline.size();
    outline.items.push_back(std::move(item));
    if (parent < 0) {
      outline.roots.push_back(index);
    } else {
      outline.items[static_cast<std::size_t>(parent)].children.push_back(index);
    }

    if (FPDF_BOOKMARK child = FPDFBookmark_GetFirstChild(doc, node); child != nullptr) {
      if (depth + 1 >= view::Outline::kMaxDepth) {
        outline.truncated = true;
      } else {
        stack.push_back({child, index, depth + 1});
      }
    }
  }
  outline.build_position_index();
  return outline;
}

Result<Point> PdfiumDocument::resolve_location(int page_index, Point user_pt) const {
  PdfiumLibrary::assert_current_thread();
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  return detail::PageSpace::of(page->get()).to_pt(user_pt.x, user_pt.y);
}

Result<view::PrintPreferences> PdfiumDocument::print_preferences() const {
  PdfiumLibrary::assert_current_thread();
  FPDF_DOCUMENT doc = doc_.get();
  view::PrintPreferences prefs;
  // 권한: 암호화되지 않았거나 소유자 암호로 열면 모든 비트가 켜져 있다
  const unsigned long perm = FPDF_GetDocPermissions(doc);
  prefs.allowed = (perm & (1UL << 2)) != 0;
  prefs.high_quality = prefs.allowed && (perm & (1UL << 11)) != 0;

  prefs.no_scaling = !FPDF_VIEWERREF_GetPrintScaling(doc);
  prefs.copies = std::clamp(FPDF_VIEWERREF_GetNumCopies(doc), 1, 999);

  const int count = page_count();
  if (FPDF_PAGERANGE range = FPDF_VIEWERREF_GetPrintPageRange(doc); range != nullptr) {
    const std::size_t n = FPDF_VIEWERREF_GetPrintPageRangeCount(range);
    // 쌍으로만 해석한다. PDF 명세상 1 기준 쪽 번호이고 PDFium 은 값을 그대로 돌려준다 → 0 기준으로 변환
    for (std::size_t i = 0; i + 1 < n && prefs.page_ranges.size() < 1000; i += 2) {
      const int first = FPDF_VIEWERREF_GetPrintPageRangeElement(range, i) - 1;
      const int last = FPDF_VIEWERREF_GetPrintPageRangeElement(range, i + 1) - 1;
      if (first < 0 || last < first || first >= count) continue;
      prefs.page_ranges.emplace_back(first, std::min(last, count - 1));
    }
  }
  switch (FPDF_VIEWERREF_GetDuplex(doc)) {
    case Simplex: prefs.duplex = view::Duplex::kSimplex; break;
    case DuplexFlipShortEdge: prefs.duplex = view::Duplex::kFlipShortEdge; break;
    case DuplexFlipLongEdge: prefs.duplex = view::Duplex::kFlipLongEdge; break;
    default: prefs.duplex = view::Duplex::kUndefined; break;
  }
  return prefs;
}

Result<view::PageSearchResult> PdfiumDocument::search_page(int page_index, const std::string& query_utf8,
                                                          view::SearchOptions options) const {
  PdfiumLibrary::assert_current_thread();
  auto page = load_page(page_index);
  if (!page) return std::unexpected(page.error());
  auto text_page = TextPage::load(page->get());
  if (!text_page) return std::unexpected(text_page.error());

  view::PageSearchResult result;
  result.page = page_index;
  result.char_count = text_page->raw_char_count();
  // 먼저 찾고, 결과가 있을 때만 인덱스를 만든다 (검색 비용의 대부분이 인덱스 추출)
  const std::vector<view::SourceRange> found = text_page->find(query_utf8, options);
  if (found.empty()) return result;
  auto index = text_page->extract(doc_.get(), page->get(), page_index);
  if (!index) return std::unexpected(index.error());
  // PDFium 검색기는 매핑 오류 글자도 PDFium 값(글자 코드)으로 맞춘다. 인덱스는 그 글자를 U+FFFD 로 두므로
  // (복사·선택과 같은 기준으로) U+FFFD 글자를 포함한 결과는 버린다. 검색어에 U+FFFD 가 있으면 그대로 둔다
  const bool query_has_replacement = query_utf8.find("\xEF\xBF\xBD") != std::string::npos;
  for (const view::SourceRange& src : found) {
    const view::TextRange range = view::map_source_range(*index, src);
    if (range.empty()) continue;
    if (!query_has_replacement) {
      const auto first = index->chars.begin() + range.start;
      const auto last = index->chars.begin() + range.end;
      if (std::any_of(first, last, [](const view::TextChar& ch) { return ch.code == U'\uFFFD'; })) continue;
    }
    result.ranges.push_back(range);
  }
  std::sort(result.ranges.begin(), result.ranges.end(),
            [](const view::TextRange& a, const view::TextRange& b) { return a.start < b.start; });
  result.rects.reserve(result.ranges.size());
  for (const view::TextRange& range : result.ranges) result.rects.push_back(index->selection_rects(range));
  return result;
}

}  // namespace onpdf::engine
