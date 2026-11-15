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
TEST(Tile, PreviewScaleFitsWholePageInOneTile) {
  EXPECT_EQ(preview_scale_q_for({612, 792}, 8), 8);       // Letter: 기본 1/8
  EXPECT_EQ(preview_scale_q_for({4096, 4096}, 8), 8);     // 경계: 512px 딱 맞음
  EXPECT_EQ(preview_scale_q_for({8192, 1000}, 8), 4);     // 긴 변 기준
  EXPECT_EQ(preview_scale_q_for({10000, 7000}, 8), 3);
  EXPECT_EQ(preview_scale_q_for({100000, 10}, 8), 1);     // 하한 1/64 (일부만 덮음)
  EXPECT_EQ(preview_scale_q_for({0, 0}, 8), 8);           // 비정상 크기: 기본값
  for (engine::PageSize pt : {engine::PageSize{4097, 100}, engine::PageSize{2384, 3370}, engine::PageSize{14400, 14400}}) {
    const int q = preview_scale_q_for(pt, 8);
    const Size px = page_pixel_size(pt, q);
    EXPECT_LE(std::max(px.w, px.h), kTileSize) << pt.width << "x" << pt.height;
  }
}

}  // namespace onpdf::view