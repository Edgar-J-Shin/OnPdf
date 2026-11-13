#pragma once

namespace onpdf::engine {

// FPDF_InitLibraryWithConfig / FPDF_DestroyLibrary 의 RAII 래퍼.
// 프로세스에서 하나만 만들고, 모든 PDFium 호출보다 먼저 생성되어 늦게 소멸해야 한다.
// PDFium 은 스레드 안전하지 않으므로 이후의 모든 PDFium 호출은 한 스레드(워커)에서만 한다.
// JavaScript 는 활성화하지 않는다 (V8 미연결).
class PdfiumLibrary {
 public:
  PdfiumLibrary();
  ~PdfiumLibrary();

  PdfiumLibrary(const PdfiumLibrary&) = delete;
  PdfiumLibrary& operator=(const PdfiumLibrary&) = delete;
  PdfiumLibrary(PdfiumLibrary&&) = delete;
  PdfiumLibrary& operator=(PdfiumLibrary&&) = delete;

  [[nodiscard]] static bool is_initialized() noexcept;
};

}  // namespace onpdf::engine
