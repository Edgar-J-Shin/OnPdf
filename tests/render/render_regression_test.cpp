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
  bool printing = false;  // FPDF_PRINTING (인쇄 플래그 주석 규칙)
  int rotation = 0;
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
  options.printing = c.printing;
  options.rotation = c.rotation;
  auto actual = doc->render_page(c.page, options);
  ASSERT_TRUE(actual) << actual.error().message;

  const auto expected_path = testdata("expected") / c.expected;
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

// 한글 CID 폰트 임베딩 — 서브셋 TrueType 이 들어 있어 플랫폼 무관하게 같은 결과
TEST(RenderRegression, KoreanEmbeddedFont) {
  expect_matches_reference({"korean-embedded.pdf", 0, 1.0f, "korean-embedded-p0.png"});
}

TEST(RenderRegression, ScannedImagePage) {
  expect_matches_reference({"scanned.pdf", 0, 0.5f, "scanned-p0.png"});
}

TEST(RenderRegression, AcroFormWidgets) {
  expect_matches_reference({"form-acro.pdf", 0, 1.0f, "form-acro-p0.png"});
}

TEST(RenderRegression, Annotations) {
  expect_matches_reference({"annotations.pdf", 0, 1.0f, "annotations-p0.png"});
}

// /Rotate 90 쪽: 가로·세로가 바뀐 비트맵
TEST(RenderRegression, RotatedPage) {
  expect_matches_reference({"mixed-sizes.pdf", 5, 0.5f, "mixed-sizes-p5.png"});
}

// 인쇄 플래그: 화면 렌더는 녹색(화면 전용)만, 인쇄 렌더는 빨강(인쇄 전용)만
TEST(RenderRegression, PrintFlagsScreen) {
  expect_matches_reference({"print-flags.pdf", 0, 0.5f, "print-flags-p0-screen.png"});
}

TEST(RenderRegression, PrintFlagsPrint) {
  expect_matches_reference({"print-flags.pdf", 0, 0.5f, "print-flags-p0-print.png", true});
}

// 인쇄 자동 회전: 가로 쪽을 반시계 90° 로 렌더
TEST(RenderRegression, PrintRotatedLandscape) {
  expect_matches_reference({"simple.pdf", 1, 0.5f, "simple-p1-rot270.png", true, 270});
}

}  // namespace
}  // namespace onpdf
