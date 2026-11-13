#include "onpdf/core/engine/pdfium_library.h"

#include <fpdf_sysfontinfo.h>
#include <fpdfview.h>

#include <atomic>
#include <cassert>

namespace onpdf::engine {

namespace {
std::atomic<int> g_instances{0};

// 모든 콜백이 비어 있는 시스템 폰트 정보. PDFium 은 null 콜백을 "없음"으로 처리하고
// 내장 폰트로 대체한다. FPDF_SetSystemFontInfo 는 포인터를 보관하므로 정적으로 둔다.
FPDF_SYSFONTINFO g_no_system_fonts{};
}  // namespace

PdfiumLibrary::PdfiumLibrary(const Options& options) {
  const int previous = g_instances.fetch_add(1);
  assert(previous == 0 && "PdfiumLibrary 는 프로세스에 하나만 둔다");
  (void)previous;

  FPDF_LIBRARY_CONFIG config{};
  config.version = 2;
  config.m_pUserFontPaths = nullptr;
  config.m_pIsolate = nullptr;  // JavaScript 비활성화
  config.m_v8EmbedderSlot = 0;
  FPDF_InitLibraryWithConfig(&config);

  if (!options.use_system_fonts) {
    g_no_system_fonts = FPDF_SYSFONTINFO{};
    g_no_system_fonts.version = 1;
    FPDF_SetSystemFontInfo(&g_no_system_fonts);
  }
}

PdfiumLibrary::~PdfiumLibrary() {
  FPDF_DestroyLibrary();
  g_instances.fetch_sub(1);
}

bool PdfiumLibrary::is_initialized() noexcept { return g_instances.load() > 0; }

}  // namespace onpdf::engine
