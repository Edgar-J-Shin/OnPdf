#include "onpdf/core/image/png_io.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace onpdf::image {
namespace {

TEST(PngIo, RoundTripPreservesPixels) {
  Bitmap bitmap;
  bitmap.width = 3;
  bitmap.height = 2;
  bitmap.stride = 12;
  bitmap.pixels = {
      // B    G    R    A
      0,   0,   255, 255, 0,   255, 0,   255, 255, 0,   0,   255,  // row 0: red, green, blue
      10,  20,  30,  255, 40,  50,  60,  255, 70,  80,  90,  255,  // row 1
  };

  const auto path = std::filesystem::temp_directory_path() / "onpdf_png_io_test.png";
  auto written = write_png(bitmap, path);
  ASSERT_TRUE(written) << written.error().message;

  auto read = read_png(path);
  ASSERT_TRUE(read) << read.error().message;
  EXPECT_EQ(read->width, 3);
  EXPECT_EQ(read->height, 2);
  EXPECT_EQ(read->pixels, bitmap.pixels);
  std::filesystem::remove(path);
}

TEST(PngIo, EmptyBitmapIsRejected) {
  auto written = write_png(Bitmap{}, std::filesystem::temp_directory_path() / "x.png");
  ASSERT_FALSE(written);
  EXPECT_EQ(written.error().code, ErrorCode::kInvalidArgument);
}

}  // namespace
}  // namespace onpdf::image
