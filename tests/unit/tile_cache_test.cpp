#include "onpdf/core/view/tile_cache.h"

#include <gtest/gtest.h>

namespace onpdf::view {
namespace {

std::shared_ptr<const Bitmap> bitmap_of(std::size_t bytes) {
  auto b = std::make_shared<Bitmap>();
  b->width = static_cast<int>(bytes / 4);
  b->height = 1;
  b->stride = b->width * 4;
  b->pixels.assign(bytes, 0);
  return b;
}

TEST(TileCache, GetMissThenHit) {
  TileCache cache(1000);
  const TileKey k{0, 64, 0, 0};
  EXPECT_EQ(cache.get(k), nullptr);
  cache.put(k, bitmap_of(100));
  EXPECT_TRUE(cache.contains(k));
  EXPECT_NE(cache.get(k), nullptr);
  EXPECT_EQ(cache.stats().hits, 1u);
  EXPECT_EQ(cache.stats().misses, 1u);
  EXPECT_EQ(cache.stats().bytes, 100u);
}

TEST(TileCache, EvictsLeastRecentlyUsed) {
  TileCache cache(300);
  const TileKey a{0, 64, 0, 0}, b{0, 64, 1, 0}, c{0, 64, 2, 0}, d{0, 64, 3, 0};
  cache.put(a, bitmap_of(100));
  cache.put(b, bitmap_of(100));
  cache.put(c, bitmap_of(100));
  (void)cache.get(a);  // a 를 최근으로
  cache.put(d, bitmap_of(100));  // 상한 초과 → 가장 오래된 b 제거
  EXPECT_TRUE(cache.contains(a));
  EXPECT_FALSE(cache.contains(b));
  EXPECT_TRUE(cache.contains(c));
  EXPECT_TRUE(cache.contains(d));
  EXPECT_EQ(cache.stats().evictions, 1u);
  EXPECT_EQ(cache.stats().count, 3u);
  EXPECT_LE(cache.stats().bytes, 300u);
}

TEST(TileCache, ReplaceAndShrink) {
  TileCache cache(1000);
  const TileKey a{1, 64, 0, 0};
  cache.put(a, bitmap_of(100));
  cache.put(a, bitmap_of(400));  // 교체
  EXPECT_EQ(cache.stats().count, 1u);
  EXPECT_EQ(cache.stats().bytes, 400u);
  cache.put({1, 32, 0, 0}, bitmap_of(100));
  EXPECT_EQ(cache.tiles_for_page(1).size(), 2u);
  EXPECT_EQ(cache.tiles_for_page(2).size(), 0u);
  cache.set_max_bytes(150);  // 줄이면 즉시 제거
  EXPECT_LE(cache.stats().bytes, 150u);
  cache.clear();
  EXPECT_EQ(cache.stats().count, 0u);
  EXPECT_EQ(cache.stats().bytes, 0u);
}

}  // namespace
}  // namespace onpdf::view
