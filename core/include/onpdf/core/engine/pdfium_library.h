#pragma once

namespace onpdf::engine {

// FPDF_InitLibraryWithConfig / FPDF_DestroyLibrary 의 RAII 래퍼.
// 프로세스에서 하나만 만들고, 모든 PDFium 호출보다 먼저 생성되어 늦게 소멸해야 한다.
// PDFium 은 스레드 안전하지 않으므로 이후의 모든 PDFium 호출은 한 스레드(워커)에서만 한다.
// JavaScript 는 활성화하지 않는다 (V8 미연결).
class PdfiumLibrary {
 public:
  struct Options {
    // false 면 시스템 폰트를 쓰지 않고 PDFium 내장(Foxit) 폰트만 쓴다.
    // 렌더링 회귀 테스트처럼 플랫폼 간 결과가 같아야 할 때 끈다. 앱에서는 켠다.
    bool use_system_fonts = true;
  };

  PdfiumLibrary() : PdfiumLibrary(Options{}) {}
  explicit PdfiumLibrary(const Options& options);
  ~PdfiumLibrary();

  PdfiumLibrary(const PdfiumLibrary&) = delete;
  PdfiumLibrary& operator=(const PdfiumLibrary&) = delete;
  PdfiumLibrary(PdfiumLibrary&&) = delete;
  PdfiumLibrary& operator=(PdfiumLibrary&&) = delete;

  [[nodiscard]] static bool is_initialized() noexcept;

  // 디버그 빌드에서 현재 스레드가 라이브러리를 만든 스레드인지 단정한다.
  // PdfiumDocument 의 공개 메서드가 호출해 워커 밖 PDFium 사용을 잡는다. 릴리스에서는 no-op.
  static void assert_current_thread() noexcept;
};

}  // namespace onpdf::engine
