#include "onpdf/core/image/png_io.h"

#include "onpdf/core/platform/path.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_WRITE_NO_STDIO
#include <stb_image.h>
#include <stb_image_write.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

namespace onpdf::image {

namespace {

void append_to_vector(void* context, void* data, int size) {
  auto* out = static_cast<std::vector<std::uint8_t>*>(context);
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  out->insert(out->end(), bytes, bytes + size);
}

}  // namespace

Result<void> write_png(const Bitmap& bitmap, const std::filesystem::path& path) {
  if (bitmap.empty()) {
    return make_error(ErrorCode::kInvalidArgument, "empty bitmap");
  }
  const std::size_t count =
      static_cast<std::size_t>(bitmap.width) * static_cast<std::size_t>(bitmap.height);
  std::vector<std::uint8_t> rgba(count * 4);
  for (int y = 0; y < bitmap.height; ++y) {
    const std::uint8_t* src = bitmap.row(y);
    std::uint8_t* dst = rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(bitmap.width) * 4;
    for (int x = 0; x < bitmap.width; ++x) {
      dst[0] = src[2];
      dst[1] = src[1];
      dst[2] = src[0];
      dst[3] = src[3];
      src += 4;
      dst += 4;
    }
  }

  std::vector<std::uint8_t> encoded;
  if (!stbi_write_png_to_func(append_to_vector, &encoded, bitmap.width, bitmap.height, 4,
                              rgba.data(), bitmap.width * 4)) {
    return make_error(ErrorCode::kIoError, "PNG encode failed");
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return make_error(ErrorCode::kIoError, "cannot write: " + path_to_utf8(path));
  }
  out.write(reinterpret_cast<const char*>(encoded.data()),
            static_cast<std::streamsize>(encoded.size()));
  if (!out) {
    return make_error(ErrorCode::kIoError, "write failed: " + path_to_utf8(path));
  }
  return {};
}

Result<Bitmap> read_png(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return make_error(ErrorCode::kFileNotFound, "cannot open: " + path_to_utf8(path));
  }
  std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
  int w = 0, h = 0, channels = 0;
  stbi_uc* rgba = stbi_load_from_memory(file.data(), static_cast<int>(file.size()), &w, &h,
                                        &channels, 4);
  if (rgba == nullptr) {
    return make_error(ErrorCode::kInvalidFormat, "PNG decode failed: " + path_to_utf8(path));
  }
  Bitmap out;
  out.width = w;
  out.height = h;
  out.stride = w * 4;
  out.pixels.resize(static_cast<std::size_t>(out.stride) * static_cast<std::size_t>(h));
  const stbi_uc* src = rgba;
  std::uint8_t* dst = out.pixels.data();
  for (std::size_t i = 0, n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h); i < n; ++i) {
    dst[0] = src[2];
    dst[1] = src[1];
    dst[2] = src[0];
    dst[3] = src[3];
    src += 4;
    dst += 4;
  }
  stbi_image_free(rgba);
  return out;
}

}  // namespace onpdf::image
