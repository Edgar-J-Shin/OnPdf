#include "onpdf/core/engine/pdfium_library.h"

#include <fpdfview.h>

#include <atomic>
#include <cassert>

namespace onpdf::engine {

namespace {
std::atomic<int> g_instances{0};
}  // namespace

PdfiumLibrary::PdfiumLibrary() {
  const int previous = g_instances.fetch_add(1);
  assert(previous == 0 && "PdfiumLibrary 는 프로세스에 하나만 둔다");
  (void)previous;

  FPDF_LIBRARY_CONFIG config{};
  config.version = 2;
  config.m_pUserFontPaths = nullptr;
  config.m_pIsolate = nullptr;  // JavaScript 비활성화
  config.m_v8EmbedderSlot = 0;
  FPDF_InitLibraryWithConfig(&config);
}

PdfiumLibrary::~PdfiumLibrary() {
  FPDF_DestroyLibrary();
  g_instances.fetch_sub(1);
}

bool PdfiumLibrary::is_initialized() noexcept { return g_instances.load() > 0; }

}  // namespace onpdf::engine
