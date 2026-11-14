#include "onpdf/core/view/tile_cache.h"

#include <gtest/gtest.h>

namespace onpdf::view {
namespace {

constexpr std::size_t kTile = std::size_t{512} * 512 * 4;  // 1MB
constexpr std::size_t kFourTiles = 4 * kTile;              // 캐시 최소 상한

std::shared_ptr<const Bitmap> bitmap_of(std::size_t bytes) {
  auto b = std::make_shared<Bitmap>();
  b->width = static_cast<int>(bytes / 4);
  b->height = 1;
  b->stride = b->width * 4;
  b->pixels.assign(bytes, 0);
  return b;
}

TEST(TileCache, GetMissThenHit) {
  TileCache cache(kFourTiles);
  const TileKey k{0, 64, 0, 0};
  EXPECT_EQ(cache.get(k), nullptr);
  cache.put(k, bitmap_of(100));
  EXPECT_TRUE(cache.contains(k));
  EXPECT_NE(cache.get(k), nullptr);
  EXPECT_EQ(cache.stats().hits, 1u);
  EXPECT_EQ(cache.stats().misses, 1u);
  EXPECT_EQ(cache.stats().bytes, 100u);
}

TEST(TileCache, MinimumBudgetIsFourTiles) {
  // 타일 하나보다 작은 상한은 put 이 자기 자신을 제거해 영원히 재요청되므로 4타일로 올린다
  TileCache cache(300);
  EXPECT_EQ(cache.max_bytes(), kFourTiles);
  cache.put({0, 64, 0, 0}, bitmap_of(kTile));
  EXPECT_TRUE(cache.contains({0, 64, 0, 0}));
}

TEST(TileCache, EvictsLeastRecentlyUsed) {
  TileCache cache(kFourTiles);
  const TileKey a{0, 64, 0, 0}, b{0, 64, 1, 0}, c{0, 64, 2, 0}, d{0, 64, 3, 0}, e{0, 64, 4, 0};
  cache.put(a, bitmap_of(kTile));
  cache.put(b, bitmap_of(kTile));
  cache.put(c, bitmap_of(kTile));
  cache.put(d, bitmap_of(kTile));  // 정확히 상한
  (void)cache.get(a);              // a 를 최근으로
  cache.put(e, bitmap_of(kTile));  // 상한 초과 → 가장 오래된 b 제거
  EXPECT_TRUE(cache.contains(a));
  EXPECT_FALSE(cache.contains(b));
  EXPECT_TRUE(cache.contains(c));
  EXPECT_TRUE(cache.contains(d));
  EXPECT_TRUE(cache.contains(e));
  EXPECT_EQ(cache.stats().evictions, 1u);
  EXPECT_EQ(cache.stats().count, 4u);
  EXPECT_LE(cache.stats().bytes, kFourTiles);
}

TEST(TileCache, ReplaceAndShrink) {
  TileCache cache(8 * kTile);
  const TileKey a{1, 64, 0, 0};
  cache.put(a, bitmap_of(100));
  cache.put(a, bitmap_of(400));  // 교체
  EXPECT_EQ(cache.stats().count, 1u);
  EXPECT_EQ(cache.stats().bytes, 400u);
  cache.put({1, 32, 0, 0}, bitmap_of(100));
  EXPECT_EQ(cache.tiles_for_page(1).size(), 2u);
  EXPECT_EQ(cache.tiles_for_page(2).size(), 0u);
  EXPECT_EQ(cache.keys().size(), 2u);
  for (int i = 0; i < 6; ++i) cache.put({2, 64, i, 0}, bitmap_of(kTile));  // 총 6MB + 500B
  cache.set_max_bytes(kFourTiles);  // 줄이면 즉시 LRU 제거
  EXPECT_LE(cache.stats().bytes, kFourTiles);
  cache.clear();
  EXPECT_EQ(cache.stats().count, 0u);
  EXPECT_EQ(cache.stats().bytes, 0u);
}

}  // namespace
}  // namespace onpdf::view
