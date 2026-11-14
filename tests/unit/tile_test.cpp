#include "onpdf/core/view/tile.h"

#include <gtest/gtest.h>

namespace onpdf::view {
namespace {

TEST(Tile, ScaleQuantization) {
  EXPECT_EQ(quantize_scale(1.0), 64);
  EXPECT_EQ(quantize_scale(1.5), 96);
  EXPECT_EQ(quantize_scale(0.001), kMinScaleQ);
  EXPECT_DOUBLE_EQ(dequantize_scale(96), 1.5);
  EXPECT_EQ(quantize_scale(dequantize_scale(123)), 123);
}

TEST(Tile, GridAndRects) {
  const engine::PageSize letter{612, 792};
  const Size px = page_pixel_size(letter, 128);  // 2.0 px/pt
  EXPECT_DOUBLE_EQ(px.w, 1224);
  EXPECT_DOUBLE_EQ(px.h, 1584);
  EXPECT_EQ(tiles_across(px.w), 3);
  EXPECT_EQ(tiles_across(px.h), 4);
  EXPECT_EQ(tiles_across(0), 0);

  EXPECT_EQ(tile_rect_px({0, 128, 0, 0}, letter), (Rect{0, 0, 512, 512}));
  EXPECT_EQ(tile_rect_px({0, 128, 2, 3}, letter), (Rect{1024, 1536, 200, 48}));  // 마지막 타일은 잘림
}

TEST(Tile, KeyHashAndOrder) {
  const TileKey a{1, 64, 0, 0};
  const TileKey b{1, 64, 0, 1};
  EXPECT_NE(a, b);
  EXPECT_LT(a, b);
  EXPECT_NE(TileKeyHash{}(a), TileKeyHash{}(b));
}

}  // namespace
}  // namespace onpdf::view
