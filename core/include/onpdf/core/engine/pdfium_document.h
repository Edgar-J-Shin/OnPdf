#pragma once

#include "onpdf/core/bitmap.h"
#include "onpdf/core/engine/pdfium_handles.h"
#include "onpdf/core/error.h"

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace onpdf::engine {

// 페이지 크기 (PDF 포인트, 1/72 inch)
struct PageSize {
  double width = 0.0;
  double height = 0.0;
};

struct RenderOptions {
  // 포인트당 픽셀 수. 1.0 = 72 DPI, 96 DPI 화면은 96/72.
  float scale = 1.0f;
  // 0, 90, 180, 270 (시계 방향)
  int rotation = 0;
  // 주석(annotation) 렌더링 여부
  bool render_annotations = true;
  // LCD 서브픽셀 텍스트
  bool lcd_text = false;
};

// 열린 PDF 문서. 이동만 가능하며 PDFium 워커 스레드에서만 사용한다.
// 파일 내용은 메모리에 통째로 올린다 (M1 에서 FPDF_LoadCustomDocument 로 전환 예정).
class PdfiumDocument {
 public:
  static Result<PdfiumDocument> open(const std::filesystem::path& path,
                                     std::string_view password = {});
  static Result<PdfiumDocument> open_from_memory(std::vector<std::uint8_t> data,
                                                 std::string_view password = {});

  PdfiumDocument(PdfiumDocument&&) noexcept = default;
  PdfiumDocument& operator=(PdfiumDocument&&) noexcept = default;
  PdfiumDocument(const PdfiumDocument&) = delete;
  PdfiumDocument& operator=(const PdfiumDocument&) = delete;
  ~PdfiumDocument() = default;

  [[nodiscard]] int page_count() const noexcept;
  [[nodiscard]] Result<PageSize> page_size(int page_index) const;
  [[nodiscard]] Result<Bitmap> render_page(int page_index, const RenderOptions& options) const;

 private:
  PdfiumDocument(std::vector<std::uint8_t> data, ScopedDocument doc);

  Result<ScopedPage> load_page(int page_index) const;

  // 선언 순서 중요: doc_ 가 data_ 를 참조하므로 doc_ 가 먼저 소멸해야 한다.
  std::vector<std::uint8_t> data_;
  ScopedDocument doc_;
};

}  // namespace onpdf::engine
