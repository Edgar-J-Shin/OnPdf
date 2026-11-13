// 렌더링 회귀 테스트: testdata/*.pdf 를 그려 testdata/expected/*.png 와 비교한다.
//
// 기준 이미지 갱신: ONPDF_UPDATE_REFERENCES=1 로 실행하면 현재 결과를 기준으로 덮어쓴다.
// 비교 허용치는 tests/test_support.h 의 expect_bitmap_matches_png 참고.

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
using onpdf::test::expect_bitmap_matches_png;
using onpdf::test::testdata;

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
  expect_bitmap_matches_png(*actual, expected_path, c.expected);
}

TEST(RenderRegression, SimplePage0) {
  expect_matches_reference({"simple.pdf", 0, 1.0f, "simple-p0.png"});
}

TEST(RenderRegression, SimplePage1Landscape) {
  expect_matches_reference({"simple.pdf", 1, 0.5f, "simple-p1.png"});
}

}  // namespace
}  // namespace onpdf
