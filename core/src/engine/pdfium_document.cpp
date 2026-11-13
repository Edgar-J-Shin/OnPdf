#include "onpdf/core/engine/pdfium_document.h"

#include "onpdf/core/engine/pdfium_library.h"

#include <fpdfview.h>

#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

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
    return make_error(ErrorCode::kFileNotFound, "file not found: " + path.string());
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return make_error(ErrorCode::kIoError, "cannot open: " + path.string());
  }
  std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
  if (in.bad()) {
    return make_error(ErrorCode::kIoError, "read failed: " + path.string());
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
  assert(PdfiumLibrary::is_initialized() && "PdfiumLibrary 를 먼저 생성해야 한다");
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

int PdfiumDocument::page_count() const noexcept { return FPDF_GetPageCount(doc_.get()); }

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
  FS_SIZEF size{};
  if (page_index < 0 || page_index >= page_count() ||
      !FPDF_GetPageSizeByIndexF(doc_.get(), page_index, &size)) {
    return make_error(ErrorCode::kPageNotFound,
                      "page index out of range: " + std::to_string(page_index));
  }
  return PageSize{size.width, size.height};
}

Result<Bitmap> PdfiumDocument::render_page(int page_index, const RenderOptions& options) const {
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

}  // namespace onpdf::engine
