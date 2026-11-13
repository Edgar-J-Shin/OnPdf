#include "onpdf/core/engine/pdfium_document.h"

#include "../test_support.h"

#include <gtest/gtest.h>

namespace onpdf::engine {
namespace {

using onpdf::test::testdata;

TEST(PdfiumDocument, OpensSimplePdf) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  EXPECT_EQ(doc->page_count(), 2);

  auto size = doc->page_size(0);
  ASSERT_TRUE(size) << size.error().message;
  EXPECT_DOUBLE_EQ(size->width, 612.0);
  EXPECT_DOUBLE_EQ(size->height, 792.0);

  auto landscape = doc->page_size(1);
  ASSERT_TRUE(landscape) << landscape.error().message;
  EXPECT_DOUBLE_EQ(landscape->width, 792.0);
  EXPECT_DOUBLE_EQ(landscape->height, 612.0);
}

TEST(PdfiumDocument, MissingFileReportsFileNotFound) {
  auto doc = PdfiumDocument::open(testdata("does-not-exist.pdf"));
  ASSERT_FALSE(doc);
  EXPECT_EQ(doc.error().code, ErrorCode::kFileNotFound);
}

TEST(PdfiumDocument, NonPdfReportsInvalidFormat) {
  auto doc = PdfiumDocument::open(testdata("not_a_pdf.txt"));
  ASSERT_FALSE(doc);
  EXPECT_EQ(doc.error().code, ErrorCode::kInvalidFormat);
}

TEST(PdfiumDocument, EmptyBufferReportsInvalidFormat) {
  auto doc = PdfiumDocument::open_from_memory({});
  ASSERT_FALSE(doc);
  EXPECT_EQ(doc.error().code, ErrorCode::kInvalidFormat);
}

TEST(PdfiumDocument, PageIndexOutOfRange) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  EXPECT_EQ(doc->page_size(-1).error().code, ErrorCode::kPageNotFound);
  EXPECT_EQ(doc->page_size(2).error().code, ErrorCode::kPageNotFound);
  EXPECT_EQ(doc->render_page(2, {}).error().code, ErrorCode::kPageNotFound);
}

TEST(PdfiumDocument, RejectsInvalidRenderOptions) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  RenderOptions bad_scale;
  bad_scale.scale = 0.0f;
  EXPECT_EQ(doc->render_page(0, bad_scale).error().code, ErrorCode::kInvalidArgument);
  RenderOptions bad_rotation;
  bad_rotation.rotation = 45;
  EXPECT_EQ(doc->render_page(0, bad_rotation).error().code, ErrorCode::kInvalidArgument);
}

TEST(PdfiumDocument, RendersPageWithContent) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;

  RenderOptions options;
  options.scale = 0.5f;
  auto bitmap = doc->render_page(0, options);
  ASSERT_TRUE(bitmap) << bitmap.error().message;
  EXPECT_EQ(bitmap->width, 306);
  EXPECT_EQ(bitmap->height, 396);
  EXPECT_EQ(bitmap->stride, 306 * 4);

  // 좌상단 모서리는 여백이라 흰색
  const std::uint8_t* corner = bitmap->row(0);
  EXPECT_EQ(corner[0], 255);
  EXPECT_EQ(corner[1], 255);
  EXPECT_EQ(corner[2], 255);

  // 페이지 어딘가에는 흰색이 아닌 픽셀(텍스트·사각형)이 있어야 한다
  bool has_ink = false;
  for (int y = 0; y < bitmap->height && !has_ink; ++y) {
    const std::uint8_t* row = bitmap->row(y);
    for (int x = 0; x < bitmap->width; ++x) {
      if (row[x * 4] != 255 || row[x * 4 + 1] != 255 || row[x * 4 + 2] != 255) {
        has_ink = true;
        break;
      }
    }
  }
  EXPECT_TRUE(has_ink);
}

TEST(PdfiumDocument, RotationSwapsDimensions) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  RenderOptions options;
  options.scale = 0.25f;
  options.rotation = 90;
  auto bitmap = doc->render_page(0, options);
  ASSERT_TRUE(bitmap) << bitmap.error().message;
  EXPECT_EQ(bitmap->width, 198);
  EXPECT_EQ(bitmap->height, 153);
}

}  // namespace
}  // namespace onpdf::engine
