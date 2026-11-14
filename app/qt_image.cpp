#include "qt_image.h"

#include <cstring>
#include <memory>
#include <utility>

namespace onpdf::app {

namespace {

void release_bitmap(void* info) { delete static_cast<std::shared_ptr<onpdf::Bitmap>*>(info); }

}  // namespace

QImage to_qimage(onpdf::Bitmap&& bitmap, qreal device_pixel_ratio) {
  if (bitmap.empty()) return QImage();
  // QImage 의 cleanup 콜백은 void* 하나만 받으므로 shared_ptr 을 힙에 하나 두고 그 포인터를 넘긴다.
  auto* owner = new std::shared_ptr<onpdf::Bitmap>(
      std::make_shared<onpdf::Bitmap>(std::move(bitmap)));
  onpdf::Bitmap& b = **owner;
  QImage image(b.pixels.data(), b.width, b.height, b.stride, QImage::Format_ARGB32, release_bitmap,
               owner);
  image.setDevicePixelRatio(device_pixel_ratio);
  return image;
}

onpdf::Bitmap to_bitmap(const QImage& image) {
  onpdf::Bitmap out;
  if (image.isNull()) return out;
  const QImage argb = image.format() == QImage::Format_ARGB32
                          ? image
                          : image.convertToFormat(QImage::Format_ARGB32);
  out.width = argb.width();
  out.height = argb.height();
  out.stride = out.width * 4;
  out.pixels.resize(static_cast<std::size_t>(out.stride) * static_cast<std::size_t>(out.height));
  for (int y = 0; y < out.height; ++y) {
    std::memcpy(out.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(out.stride),
                argb.constScanLine(y), static_cast<std::size_t>(out.stride));
  }
  return out;
}

}  // namespace onpdf::app
