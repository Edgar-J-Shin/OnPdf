#pragma once

#include "onpdf/core/bitmap.h"
#include "onpdf/core/error.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace onpdf::image {

// BGRA 비트맵을 RGBA PNG 로 저장한다.
Result<void> write_png(const Bitmap& bitmap, const std::filesystem::path& path);

// PNG 를 읽어 BGRA 비트맵으로 돌려준다 (테스트·비교용).
Result<Bitmap> read_png(const std::filesystem::path& path);

// BGRA 비트맵(알파 무시)을 baseline JPEG 바이트로 (테스트·CLI 용 — 앱은 원본 JPEG 를 그대로 쓴다, #49)
Result<std::vector<std::uint8_t>> encode_jpeg(const Bitmap& bitmap, int quality = 90);

}  // namespace onpdf::image
