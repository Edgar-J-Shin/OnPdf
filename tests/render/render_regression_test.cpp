// 렌더링 회귀 테스트: testdata/*.pdf 를 그려 testdata/expected/*.png 와 비교한다.
//
// 기준 이미지 갱신: ONPDF_UPDATE_REFERENCES=1 로 실행하면 현재 결과를 기준으로 덮어쓴다.
// 비교 허용치는 tests/test_support.h 의 expect_bitmap_matches_png 참고.

#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/edit/add_text_command.h"
#include "onpdf/core/engine/save.h"
#include "onpdf/core/image/png_io.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace onpdf {
namespace {

using engine::AnnotationKind;
using engine::AnnotationSpec;
using engine::Color;
using engine::kAnnotFlagNoRotate;
using engine::kAnnotFlagNoZoom;
using engine::kAnnotFlagPrint;
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

// 편집 → 저장 → 다시 열기: 회전해 저장한 쪽이 다른 뷰어와 같은 모양으로 그려진다 (#31)
TEST(RenderRegression, RotatedAndSavedPage) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  ASSERT_TRUE(doc->set_page_rotation(0, 1));
  onpdf::test::ScopedTempDir dir;
  const auto saved_path = dir / "rotated.pdf";
  auto saved = engine::save_document_safely(*doc, saved_path, {});
  ASSERT_TRUE(saved) << saved.error().message;
  auto reopened = PdfiumDocument::open(saved_path);
  ASSERT_TRUE(reopened) << reopened.error().message;
  RenderOptions options;
  options.scale = 0.5f;
  auto actual = reopened->render_page(0, options);
  ASSERT_TRUE(actual) << actual.error().message;
  const auto expected_path = testdata("expected") / "simple-p0-rotated-saved.png";
  if (update_references()) {
    ASSERT_TRUE(image::write_png(*actual, expected_path));
    GTEST_SKIP() << "reference updated: " << expected_path;
  }
  onpdf::test::expect_bitmap_matches_png(*actual, expected_path, "simple-p0-rotated-saved.png");
}


// 쪽 이동(마지막 쪽을 맨 앞으로) 뒤 저장한 문서의 첫 쪽: 저장이 쪽 순서를 보존한다
TEST(RenderRegression, MovedAndSavedPage) {
  auto doc = PdfiumDocument::open(testdata("mixed-sizes.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  ASSERT_TRUE(doc->move_pages({6}, 0));
  onpdf::test::ScopedTempDir dir;
  const auto saved_path = dir / "moved.pdf";
  auto saved = engine::save_document_safely(*doc, saved_path, {});
  ASSERT_TRUE(saved) << saved.error().message;
  auto reopened = PdfiumDocument::open(saved_path);
  ASSERT_TRUE(reopened) << reopened.error().message;
  ASSERT_EQ(reopened->page_count(), 7);
  RenderOptions options;
  options.scale = 0.5f;
  auto actual = reopened->render_page(0, options);
  ASSERT_TRUE(actual) << actual.error().message;
  const auto expected_path = testdata("expected") / "mixed-sizes-p0-moved-saved.png";
  if (update_references()) {
    ASSERT_TRUE(image::write_png(*actual, expected_path));
    GTEST_SKIP() << "reference updated: " << expected_path;
  }
  onpdf::test::expect_bitmap_matches_png(*actual, expected_path, "mixed-sizes-p0-moved-saved.png");
}


// 주석 5종(하이라이트·밑줄·취소선·메모·펜)을 달아 저장한 문서의 첫 쪽: PDFium 이 만든 모양이 기준과 같다
TEST(RenderRegression, AnnotatedAndSavedPage) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  auto text = doc->load_text(0);
  ASSERT_TRUE(text);
  const int n = text->char_count();
  auto markup = [&](AnnotationKind kind, int from, int to, Color color) {
    AnnotationSpec s;
    s.kind = kind;
    s.quads = text->selection_rects({std::min(from, n), std::min(to, n)});
    s.color = color;
    ASSERT_TRUE(doc->add_annotation(0, s));
  };
  markup(AnnotationKind::kHighlight, 0, 8, {255, 255, 0, 255});
  markup(AnnotationKind::kUnderline, 10, 18, {0, 0, 255, 255});
  markup(AnnotationKind::kStrikeOut, 20, 28, {255, 0, 0, 255});
  AnnotationSpec note;
  note.kind = AnnotationKind::kText;
  note.rect = {420, 80, 20, 20};
  note.flags = kAnnotFlagPrint | kAnnotFlagNoZoom | kAnnotFlagNoRotate;
  note.color = {255, 200, 0, 255};
  note.contents = "메모";
  ASSERT_TRUE(doc->add_annotation(0, note));
  AnnotationSpec ink;
  ink.kind = AnnotationKind::kInk;
  ink.ink = {{{100, 400}, {140, 380}, {180, 400}, {220, 380}, {260, 400}}};
  ink.rect = {98, 378, 164, 24};
  ink.border_width = 2.0;
  ink.color = {200, 0, 120, 255};
  ASSERT_TRUE(doc->add_annotation(0, ink));
  AnnotationSpec square;  // 도형 (#39): 두께 4, 반투명 빨강, 채움 없음
  square.kind = AnnotationKind::kSquare;
  square.rect = {80, 450, 180, 100};
  square.border_width = 4.0;
  square.color = {255, 0, 0, 128};
  ASSERT_TRUE(doc->add_annotation(0, square));
  AnnotationSpec circle;  // 두께 2, 파랑, 연한 채움
  circle.kind = AnnotationKind::kCircle;
  circle.rect = {300, 450, 120, 120};
  circle.border_width = 2.0;
  circle.color = {0, 0, 255, 255};
  circle.interior = Color{220, 230, 255, 255};
  ASSERT_TRUE(doc->add_annotation(0, circle));
  onpdf::test::ScopedTempDir dir;
  const auto saved_path = dir / "annotated.pdf";
  ASSERT_TRUE(engine::save_document_safely(*doc, saved_path, {}));
  auto reopened = PdfiumDocument::open(saved_path);
  ASSERT_TRUE(reopened) << reopened.error().message;
  RenderOptions options;
  options.scale = 1.0f;
  auto actual = reopened->render_page(0, options);
  ASSERT_TRUE(actual) << actual.error().message;
  const auto expected_path = testdata("expected") / "simple-p0-annotated.png";
  if (update_references()) {
    ASSERT_TRUE(image::write_png(*actual, expected_path));
    GTEST_SKIP() << "reference updated: " << expected_path;
  }
  onpdf::test::expect_bitmap_matches_png(*actual, expected_path, "simple-p0-annotated.png");
}

// 텍스트 추가(#45): 서브셋 임베딩 폰트만으로(시스템 폰트 없음) 한글·영문이 그려진다. 저장 → 다시 열어 렌더
TEST(RenderRegression, TextAddedAndSavedPage) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  auto face = edit::FontFace::load(testdata("fonts") / "NotoSansKR-subset.ttf");
  ASSERT_TRUE(face);
  edit::TextBoxSpec big;
  big.page = 0;
  big.origin = {72, 420};
  big.size = 24;
  big.color = {0, 0, 0, 255};
  big.utf8 = "한글 텍스트 추가\n둘째 줄 임베딩";
  big.face = *face;
  edit::AddTextCommand add_big(big);
  ASSERT_TRUE(add_big.execute(*doc));
  edit::TextBoxSpec small = big;
  small.origin = {72, 520};
  small.size = 12;
  small.color = {200, 0, 0, 255};
  small.utf8 = "OnPdf 2026 빨강 12pt";
  edit::AddTextCommand add_small(small);
  ASSERT_TRUE(add_small.execute(*doc));
  onpdf::test::ScopedTempDir dir;
  const auto saved_path = dir / "text-added.pdf";
  ASSERT_TRUE(engine::save_document_safely(*doc, saved_path, {}));
  auto reopened = PdfiumDocument::open(saved_path);
  ASSERT_TRUE(reopened) << reopened.error().message;
  RenderOptions options;
  options.scale = 1.0f;
  auto actual = reopened->render_page(0, options);
  ASSERT_TRUE(actual) << actual.error().message;
  const auto expected_path = testdata("expected") / "simple-p0-text-added.png";
  if (update_references()) {
    ASSERT_TRUE(image::write_png(*actual, expected_path));
    GTEST_SKIP() << "reference updated: " << expected_path;
  }
  onpdf::test::expect_bitmap_matches_png(*actual, expected_path, "simple-p0-text-added.png");
}

// 회전(90°)된 쪽에 넣은 텍스트: 화면 기준으로 바로 선다
TEST(RenderRegression, RotatedPageTextAdded) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  ASSERT_TRUE(doc->set_page_rotation(0, 1));
  auto face = edit::FontFace::load(testdata("fonts") / "NotoSansKR-subset.ttf");
  ASSERT_TRUE(face);
  edit::TextBoxSpec spec;
  spec.page = 0;
  spec.origin = {100, 100};
  spec.size = 24;
  spec.utf8 = "회전 쪽 텍스트";
  spec.face = *face;
  edit::AddTextCommand add(spec);
  ASSERT_TRUE(add.execute(*doc));
  RenderOptions options;
  options.scale = 1.0f;
  auto actual = doc->render_page(0, options);
  ASSERT_TRUE(actual) << actual.error().message;
  const auto expected_path = testdata("expected") / "rotated-p0-text-added.png";
  if (update_references()) {
    ASSERT_TRUE(image::write_png(*actual, expected_path));
    GTEST_SKIP() << "reference updated: " << expected_path;
  }
  onpdf::test::expect_bitmap_matches_png(*actual, expected_path, "rotated-p0-text-added.png");
}

// 회전(90°)된 쪽의 하이라이트: 글자 위에 정확히 놓인다
TEST(RenderRegression, RotatedPageHighlight) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  ASSERT_TRUE(doc->set_page_rotation(0, 1));
  auto text = doc->load_text(0);  // 회전 반영 쪽 포인트
  ASSERT_TRUE(text);
  AnnotationSpec s;
  s.kind = AnnotationKind::kHighlight;
  s.quads = text->selection_rects({0, std::min(12, text->char_count())});
  ASSERT_TRUE(doc->add_annotation(0, s));
  RenderOptions options;
  options.scale = 1.0f;
  auto actual = doc->render_page(0, options);
  ASSERT_TRUE(actual) << actual.error().message;
  const auto expected_path = testdata("expected") / "simple-p0-rotated-highlight.png";
  if (update_references()) {
    ASSERT_TRUE(image::write_png(*actual, expected_path));
    GTEST_SKIP() << "reference updated: " << expected_path;
  }
  onpdf::test::expect_bitmap_matches_png(*actual, expected_path, "simple-p0-rotated-highlight.png");
}

}  // namespace
}  // namespace onpdf
