#include "onpdf/core/view/tile_cache.h"

#include <cassert>

namespace onpdf::view {

TileCache::TileCache(std::size_t max_bytes) : max_bytes_(max_bytes) {}

std::shared_ptr<const Bitmap> TileCache::get(const TileKey& key) {
  auto it = map_.find(key);
  if (it == map_.end()) {
    ++stats_.misses;
    return nullptr;
  }
  ++stats_.hits;
  lru_.splice(lru_.begin(), lru_, it->second.lru);
  return it->second.bitmap;
}

void TileCache::put(const TileKey& key, std::shared_ptr<const Bitmap> bitmap) {
  assert(bitmap);
  const std::size_t bytes = bitmap->pixels.size();
  auto it = map_.find(key);
  if (it != map_.end()) {
    stats_.bytes -= it->second.bytes;
    it->second.bitmap = std::move(bitmap);
    it->second.bytes = bytes;
    stats_.bytes += bytes;
    lru_.splice(lru_.begin(), lru_, it->second.lru);
  } else {
    lru_.push_front(key);
    map_.emplace(key, Entry{std::move(bitmap), lru_.begin(), bytes});
    stats_.bytes += bytes;
    stats_.count = map_.size();
  }
  evict_to(max_bytes_);
}

std::vector<std::pair<TileKey, std::shared_ptr<const Bitmap>>> TileCache::tiles_for_page(int page) const {
  std::vector<std::pair<TileKey, std::shared_ptr<const Bitmap>>> out;
  for (const auto& [key, entry] : map_) {
    if (key.page == page) out.emplace_back(key, entry.bitmap);
  }
  return out;
}

std::vector<TileKey> TileCache::keys() const {
  std::vector<TileKey> out;
  out.reserve(map_.size());
  for (const auto& [key, entry] : map_) out.push_back(key);
  return out;
}

void TileCache::clear() {
  lru_.clear();
  map_.clear();
  stats_.bytes = 0;
  stats_.count = 0;
}

void TileCache::set_max_bytes(std::size_t max_bytes) {
  max_bytes_ = max_bytes;
  evict_to(max_bytes_);
}

void TileCache::evict_to(std::size_t limit) {
  while (stats_.bytes > limit && !lru_.empty()) {
    const TileKey victim = lru_.back();
    auto it = map_.find(victim);
    assert(it != map_.end());
    stats_.bytes -= it->second.bytes;
    map_.erase(it);
    lru_.pop_back();
    ++stats_.evictions;
  }
  stats_.count = map_.size();
}

}  // namespace onpdf::view
