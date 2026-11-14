#include "qt_image.h"

#include <gtest/gtest.h>

#include <QImage>

namespace onpdf::app {
namespace {

Bitmap sample_bitmap() {
  Bitmap b;
  b.width = 3;
  b.height = 2;
  b.stride = 12;
  b.pixels = {
      // B    G    R    A
      0,   0,   255, 255, 0,   255, 0,   255, 255, 0,   0,   255,  // red, green, blue
      10,  20,  30,  255, 40,  50,  60,  255, 70,  80,  90,  255,
  };
  return b;
}

TEST(QtImage, WrapsBgraWithoutCopy) {
  Bitmap b = sample_bitmap();
  const uchar* original = b.pixels.data();
  QImage image = to_qimage(std::move(b), 2.0);
  ASSERT_FALSE(image.isNull());
  EXPECT_EQ(image.format(), QImage::Format_ARGB32);
  EXPECT_EQ(image.width(), 3);
  EXPECT_EQ(image.height(), 2);
  EXPECT_EQ(image.constBits(), original);  // 복사 없음
  EXPECT_DOUBLE_EQ(image.devicePixelRatio(), 2.0);
  EXPECT_EQ(image.pixel(0, 0), qRgb(255, 0, 0));
  EXPECT_EQ(image.pixel(1, 0), qRgb(0, 255, 0));
  EXPECT_EQ(image.pixel(2, 0), qRgb(0, 0, 255));
  EXPECT_EQ(image.pixel(1, 1), qRgb(60, 50, 40));

  // 복사본이 버퍼를 공유하고, 원본이 사라져도 복사본은 유효하다
  QImage copy = image;
  image = QImage();
  EXPECT_EQ(copy.pixel(2, 1), qRgb(90, 80, 70));
}

TEST(QtImage, EmptyBitmapGivesNullImage) {
  EXPECT_TRUE(to_qimage(Bitmap{}, 1.0).isNull());
  EXPECT_TRUE(to_bitmap(QImage()).empty());
}

TEST(QtImage, RoundTripBitmap) {
  Bitmap original = sample_bitmap();
  const auto expected = original.pixels;
  QImage image = to_qimage(std::move(original), 1.0);
  Bitmap back = to_bitmap(image);
  EXPECT_EQ(back.width, 3);
  EXPECT_EQ(back.height, 2);
  EXPECT_EQ(back.stride, 12);
  EXPECT_EQ(back.pixels, expected);
}

TEST(QtImage, ToBitmapConvertsOtherFormats) {
  QImage rgb(2, 1, QImage::Format_RGB888);
  rgb.setPixel(0, 0, qRgb(1, 2, 3));
  rgb.setPixel(1, 0, qRgb(250, 251, 252));
  Bitmap b = to_bitmap(rgb);
  ASSERT_EQ(b.pixels.size(), 8u);
  EXPECT_EQ(b.pixels[0], 3);    // B
  EXPECT_EQ(b.pixels[1], 2);    // G
  EXPECT_EQ(b.pixels[2], 1);    // R
  EXPECT_EQ(b.pixels[3], 255);  // A
  EXPECT_EQ(b.pixels[6], 250);
}

}  // namespace
}  // namespace onpdf::app
