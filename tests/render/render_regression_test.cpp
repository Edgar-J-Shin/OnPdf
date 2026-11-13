// 렌더링 회귀 테스트: testdata/*.pdf 를 그려 testdata/expected/*.png 와 비교한다.
//
// 기준 이미지 갱신: ONPDF_UPDATE_REFERENCES=1 로 실행하면 현재 결과를 기준으로 덮어쓴다.
// 플랫폼별 안티에일리어싱 차이를 허용하기 위해 채널 차이 ≤ kChannelTolerance 는 같은 것으로 보고,
// 그 이상 다른 픽셀이 전체의 kMaxDifferentFraction 이하이면 통과한다.

#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/image/png_io.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace onpdf {
namespace {

using engine::PdfiumDocument;
using engine::RenderOptions;
using onpdf::test::testdata;

constexpr int kChannelTolerance = 8;
constexpr double kMaxDifferentFraction = 0.002;

struct RenderCase {
  const char* pdf;
  int page;
  float scale;
  const char* expected;
};

bool update_references() {
  const char* v = std::getenv("ONPDF_UPDATE_REFERENCES");
  return v != nullptr && std::string(v) == "1";
}

void expect_matches_reference(const RenderCase& c) {
  auto doc = PdfiumDocument::open(testdata(c.pdf));
  ASSERT_TRUE(doc) << doc.error().message;
  RenderOptions options;
  options.scale = c.scale;
  auto actual = doc->render_page(c.page, options);
  ASSERT_TRUE(actual) << actual.error().message;

  const auto expected_path = std::filesystem::path(ONPDF_TESTDATA_DIR) / "expected" / c.expected;
  if (update_references()) {
    auto written = image::write_png(*actual, expected_path);
    ASSERT_TRUE(written) << written.error().message;
    GTEST_SKIP() << "reference updated: " << expected_path;
  }

  auto expected = image::read_png(expected_path);
  ASSERT_TRUE(expected) << expected.error().message
                        << " (ONPDF_UPDATE_REFERENCES=1 로 기준 이미지를 만드세요)";
  ASSERT_EQ(actual->width, expected->width);
  ASSERT_EQ(actual->height, expected->height);

  std::size_t different = 0;
  const std::size_t total =
      static_cast<std::size_t>(actual->width) * static_cast<std::size_t>(actual->height);
  for (int y = 0; y < actual->height; ++y) {
    const std::uint8_t* a = actual->row(y);
    const std::uint8_t* e = expected->row(y);
    for (int x = 0; x < actual->width; ++x) {
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
    const auto actual_path = std::filesystem::temp_directory_path() /
                             (std::string("onpdf_actual_") + c.expected);
    (void)image::write_png(*actual, actual_path);
    FAIL() << different << " / " << total << " pixels differ (" << fraction * 100.0
           << "%). actual saved to " << actual_path;
  }
}

TEST(RenderRegression, SimplePage0) {
  expect_matches_reference({"simple.pdf", 0, 1.0f, "simple-p0.png"});
}

TEST(RenderRegression, SimplePage1Landscape) {
  expect_matches_reference({"simple.pdf", 1, 0.5f, "simple-p1.png"});
}

}  // namespace
}  // namespace onpdf
