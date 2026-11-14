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

#include <cstdlib>
#include <cstring>

namespace onpdf {
namespace {

using engine::PdfiumDocument;
using engine::RenderOptions;
using onpdf::test::testdata;
using view::SyncTileSource;
using view::TileKey;

void expect_tiles_match_full_render(const char* file, int page, double scale) {
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
  EXPECT_LE(fraction, 0.001) << file << " page " << page << " @" << scale << ": " << differing << " / " << total
                             << " pixels differ";
}

TEST(TileStitch, SimplePageAtScale2) { expect_tiles_match_full_render("simple.pdf", 0, 2.0); }
TEST(TileStitch, KoreanEmbeddedAtScale1_5) { expect_tiles_match_full_render("korean-embedded.pdf", 0, 1.5); }
TEST(TileStitch, RotatedPageAtScale1) { expect_tiles_match_full_render("mixed-sizes.pdf", 5, 1.0); }
TEST(TileStitch, ScannedPageAtScale0_7) { expect_tiles_match_full_render("scanned.pdf", 0, 0.7); }

}  // namespace
}  // namespace onpdf
