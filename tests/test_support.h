#pragma once

#include "onpdf/core/bitmap.h"
#include "onpdf/core/engine/pdfium_library.h"
#include "onpdf/core/image/png_io.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

namespace onpdf::test {

inline std::filesystem::path testdata(const char* name) {
  return std::filesystem::path(ONPDF_TESTDATA_DIR) / name;
}

// 렌더링 비교 허용치. 플랫폼별 안티에일리어싱 차이를 흡수한다.
constexpr int kChannelTolerance = 8;
constexpr double kMaxDifferentFraction = 0.002;

// 비트맵이 기준 PNG 와 허용치 안에서 일치하는지 검사한다.
// 실패 시 실제 결과를 임시 디렉터리에 저장해 경로를 알려 준다.
inline void expect_bitmap_matches_png(const Bitmap& actual, const std::filesystem::path& expected_path,
                                      const std::string& label) {
  auto expected = image::read_png(expected_path);
  ASSERT_TRUE(expected) << expected.error().message
                        << " (ONPDF_UPDATE_REFERENCES=1 로 기준 이미지를 만드세요)";
  ASSERT_EQ(actual.width, expected->width) << label;
  ASSERT_EQ(actual.height, expected->height) << label;

  std::size_t different = 0;
  const std::size_t total =
      static_cast<std::size_t>(actual.width) * static_cast<std::size_t>(actual.height);
  for (int y = 0; y < actual.height; ++y) {
    const std::uint8_t* a = actual.row(y);
    const std::uint8_t* e = expected->row(y);
    for (int x = 0; x < actual.width; ++x) {
      for (int ch = 0; ch < 3; ++ch) {
        const int diff = std::abs(int(a[x * 4 + ch]) - int(e[x * 4 + ch]));
        if (diff > kChannelTolerance) {
          ++different;
          break;
        }
      }
    }
  }
  const double fraction = static_cast<double>(different) / static_cast<double>(total);
  if (fraction > kMaxDifferentFraction) {
    const auto actual_path =
        std::filesystem::temp_directory_path() / ("onpdf_actual_" + label + ".png");
    (void)image::write_png(actual, actual_path);
    FAIL() << label << ": " << different << " / " << total << " pixels differ ("
           << fraction * 100.0 << "%). actual saved to " << actual_path;
  }
}

#ifdef ONPDF_TEST_GLOBAL_PDFIUM
// 테스트 프로세스 전체에서 PDFium 을 메인 스레드에 한 번만 초기화한다.
// 시스템 폰트를 끄고 내장 폰트만 써서 플랫폼(Windows/macOS/Linux)에 관계없이 같은 렌더링을 얻는다.
// PdfiumWorker 를 쓰는 테스트 바이너리는 이 매크로를 정의하지 않는다 (라이브러리 인스턴스는 하나만).
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
#endif

}  // namespace onpdf::test
