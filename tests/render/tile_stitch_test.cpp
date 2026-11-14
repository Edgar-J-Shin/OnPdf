// 타일 렌더 검증: 쪽을 타일로 나눠 그린 뒤 이어 붙인 결과가 "쪽 전체를 한 영역으로 render_region"
// 한 결과와 거의 같아야 한다.
//
// 완전 동일은 아니다: PDFium(AGG) 은 경로를 클립 상자에 맞춰 자르면서 새 정점을 만들기 때문에
// 평행이동된 타일에서는 대각선·글자 가장자리의 안티에일리어싱 커버리지가 몇 픽셀 달라진다
// (simple.pdf @2.0: 1.9M 픽셀 중 479개, 0.025%). 타일 경계가 어긋나거나 빠지면 수천 픽셀이 달라지므로
// 0.1% 상한으로 그 종류의 버그를 잡는다. (render_page 는 비트맵 크기를 반올림하고 다른 변환 경로를
// 타므로 비교 대상이 아니다)

#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/view/tile.h"
#include "onpdf/core/view/tile_source.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace onpdf {
namespace {

using engine::PdfiumDocument;
using engine::RenderOptions;
using onpdf::test::testdata;
using view::SyncTileSource;
using view::TileKey;

void expect_tiles_match_full_render(const char* file, int page, double scale, double max_fraction = 0.001) {
  auto doc = PdfiumDocument::open(testdata(file));
  ASSERT_TRUE(doc) << doc.error().message;
  const int scale_q = view::quantize_scale(scale);
  RenderOptions options;
  options.scale = static_cast<float>(view::dequantize_scale(scale_q));
  auto size = doc->page_size(page);
  ASSERT_TRUE(size);
  const Size px = view::page_pixel_size(*size, scale_q);
  auto full = doc->render_region(page, options, Rect{0, 0, px.w, px.h});
  ASSERT_TRUE(full) << full.error().message;
  ASSERT_EQ(full->width, static_cast<int>(px.w));
  ASSERT_EQ(full->height, static_cast<int>(px.h));

  Bitmap stitched;
  stitched.width = full->width;
  stitched.height = full->height;
  stitched.stride = full->stride;
  stitched.pixels.assign(full->pixels.size(), 0);

  SyncTileSource source(*doc);
  int tiles = 0;
  for (int ty = 0; ty < view::tiles_across(px.h); ++ty) {
    for (int tx = 0; tx < view::tiles_across(px.w); ++tx) {
      const TileKey key{page, scale_q, tx, ty};
      source.request({key, *size}, engine::Priority::kHigh, [&](const TileKey&, Result<Bitmap> r) {
        ASSERT_TRUE(r) << r.error().message;
        const Rect rect = view::tile_rect_px(key, *size);
        ASSERT_EQ(r->width, static_cast<int>(rect.w));
        ASSERT_EQ(r->height, static_cast<int>(rect.h));
        for (int y = 0; y < r->height; ++y) {
          std::memcpy(stitched.pixels.data() +
                          (static_cast<std::size_t>(static_cast<int>(rect.y) + y) * static_cast<std::size_t>(stitched.stride)) +
                          static_cast<std::size_t>(rect.x) * 4,
                      r->row(y), static_cast<std::size_t>(r->width) * 4);
        }
        ++tiles;
      });
    }
  }
  EXPECT_GE(tiles, 2);
  ASSERT_EQ(stitched.pixels.size(), full->pixels.size());
  std::size_t differing = 0;
  const std::size_t total = static_cast<std::size_t>(full->width) * static_cast<std::size_t>(full->height);
  for (std::size_t i = 0; i < full->pixels.size(); i += 4) {
    for (int ch = 0; ch < 3; ++ch) {
      if (std::abs(int(stitched.pixels[i + static_cast<std::size_t>(ch)]) - int(full->pixels[i + static_cast<std::size_t>(ch)])) > 8) {
        ++differing;
        break;
      }
    }
  }
  const double fraction = static_cast<double>(differing) / static_cast<double>(total);
  EXPECT_LE(fraction, max_fraction) << file << " page " << page << " @" << scale << ": " << differing << " / "
                                    << total << " pixels differ";
}

TEST(TileStitch, SimplePageAtScale2) { expect_tiles_match_full_render("simple.pdf", 0, 2.0); }
TEST(TileStitch, KoreanEmbeddedAtScale1_5) { expect_tiles_match_full_render("korean-embedded.pdf", 0, 1.5); }
TEST(TileStitch, RotatedPageAtScale1) { expect_tiles_match_full_render("mixed-sizes.pdf", 5, 1.0); }
// 노이즈 이미지 쪽은 전면이 조밀해서 1px 이음새 어긋남이 수십 % 를 바꾼다 → 성긴 글자 쪽보다 훨씬 민감한
// 정렬 검출기다. 상한도 더 조인다
TEST(TileStitch, ScannedPageAtScale0_7) { expect_tiles_match_full_render("scanned.pdf", 0, 0.7, 0.0005); }

// render_page(FPDF_RenderPageBitmap) 와 render_region(FPDF_RenderPageBitmapWithMatrix, 쪽 전체) 는 변환
// 경로가 달라 AA 차이는 있지만 정렬은 같아야 한다. PDFium 업데이트로 GetDisplayMatrix 가 바뀌면 여기서 드러난다
// 내용의 바운딩 박스(흰색이 아닌 픽셀 범위)를 비교한다. AA 차이에는 둔감하고 1px 어긋남에는 민감하다.
struct Bbox { int x0, y0, x1, y1; };
Bbox content_bbox(const Bitmap& b) {
  Bbox box{b.width, b.height, -1, -1};
  for (int y = 0; y < b.height; ++y) {
    const std::uint8_t* row = b.row(y);
    for (int x = 0; x < b.width; ++x) {
      if (row[x * 4] < 200 || row[x * 4 + 1] < 200 || row[x * 4 + 2] < 200) {
        box.x0 = std::min(box.x0, x);
        box.y0 = std::min(box.y0, y);
        box.x1 = std::max(box.x1, x);
        box.y1 = std::max(box.y1, y);
      }
    }
  }
  return box;
}

TEST(TileStitch, RegionMatchesPageRenderAlignment) {
  auto doc = PdfiumDocument::open(testdata("korean-embedded.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  RenderOptions options;
  options.scale = 2.0f;
  auto page = doc->render_page(0, options);
  ASSERT_TRUE(page) << page.error().message;
  auto size = doc->page_size(0);
  ASSERT_TRUE(size);
  const Size px = view::page_pixel_size(*size, view::quantize_scale(2.0));
  auto region = doc->render_region(0, options, Rect{0, 0, px.w, px.h});
  ASSERT_TRUE(region) << region.error().message;
  // 크기는 반올림/올림 차이로 최대 1px 다를 수 있다
  EXPECT_LE(std::abs(page->width - region->width), 1);
  EXPECT_LE(std::abs(page->height - region->height), 1);
  const Bbox a = content_bbox(*page);
  const Bbox b = content_bbox(*region);
  ASSERT_GE(a.x1, 0);
  EXPECT_LE(std::abs(a.x0 - b.x0), 1);
  EXPECT_LE(std::abs(a.y0 - b.y0), 1);
  EXPECT_LE(std::abs(a.x1 - b.x1), 1);
  EXPECT_LE(std::abs(a.y1 - b.y1), 1);
}

}  // namespace
}  // namespace onpdf
