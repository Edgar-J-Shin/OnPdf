#pragma once

#include "onpdf/core/engine/pdfium_library.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace onpdf::test {

inline std::filesystem::path testdata(const char* name) {
  return std::filesystem::path(ONPDF_TESTDATA_DIR) / name;
}

// 테스트 프로세스 전체에서 PDFium 을 한 번만 초기화한다.
// 시스템 폰트를 끄고 내장 폰트만 써서 플랫폼(Windows/macOS/Linux)에 관계없이 같은 렌더링을 얻는다.
class PdfiumEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    engine::PdfiumLibrary::Options options;
    options.use_system_fonts = false;
    library_ = std::make_unique<engine::PdfiumLibrary>(options);
  }
  void TearDown() override { library_.reset(); }

 private:
  std::unique_ptr<engine::PdfiumLibrary> library_;
};

inline const auto* const kPdfiumEnvironment =
    ::testing::AddGlobalTestEnvironment(new PdfiumEnvironment);

}  // namespace onpdf::test
