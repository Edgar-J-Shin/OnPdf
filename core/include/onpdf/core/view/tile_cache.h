#pragma once

// 바이트 상한 LRU 타일 캐시. 메인 스레드 전용(잠금 없음).

#include "onpdf/core/bitmap.h"
#include "onpdf/core/view/tile.h"

#include <cstddef>
#include <list>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace onpdf::view {

class TileCache {
 public:
  struct Stats {
    std::size_t bytes = 0;
    std::size_t count = 0;
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t evictions = 0;
  };

  explicit TileCache(std::size_t max_bytes = std::size_t{256} << 20);

  // LRU 갱신. 없으면 nullptr
  [[nodiscard]] std::shared_ptr<const Bitmap> get(const TileKey& key);
  [[nodiscard]] bool contains(const TileKey& key) const noexcept { return map_.contains(key); }
  // 같은 키가 있으면 교체. 상한 초과 시 가장 오래된 것부터 제거
  void put(const TileKey& key, std::shared_ptr<const Bitmap> bitmap);
  // 한 쪽의 모든 타일 (배율 불문). 대체 타일(이전 배율·미리보기) 그리기용. LRU 갱신 없음
  [[nodiscard]] std::vector<std::pair<TileKey, std::shared_ptr<const Bitmap>>> tiles_for_page(int page) const;
  // 모든 키 (LRU 순서 아님). 테스트·진단용
  [[nodiscard]] std::vector<TileKey> keys() const;
  void clear();
  void set_max_bytes(std::size_t max_bytes);
  [[nodiscard]] std::size_t max_bytes() const noexcept { return max_bytes_; }
  [[nodiscard]] Stats stats() const noexcept { return stats_; }

 private:
  struct Entry {
    std::shared_ptr<const Bitmap> bitmap;
    std::list<TileKey>::iterator lru;
    std::size_t bytes = 0;
  };
  void evict_to(std::size_t limit);

  std::size_t max_bytes_;
  std::list<TileKey> lru_;  // front = 가장 최근
  std::unordered_map<TileKey, Entry, TileKeyHash> map_;
  Stats stats_;
};

}  // namespace onpdf::view
