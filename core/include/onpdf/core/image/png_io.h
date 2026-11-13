#pragma once

#include "onpdf/core/bitmap.h"
#include "onpdf/core/error.h"

#include <filesystem>

namespace onpdf::image {

// BGRA 비트맵을 RGBA PNG 로 저장한다.
Result<void> write_png(const Bitmap& bitmap, const std::filesystem::path& path);

// PNG 를 읽어 BGRA 비트맵으로 돌려준다 (테스트·비교용).
Result<Bitmap> read_png(const std::filesystem::path& path);

}  // namespace onpdf::image
