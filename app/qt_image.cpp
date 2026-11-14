#include "qt_image.h"

#include <cstring>
#include <memory>
#include <utility>

namespace onpdf::app {

namespace {

void release_bitmap(void* info) { delete static_cast<onpdf::Bitmap*>(info); }
void release_shared_bitmap(void* info) { delete static_cast<std::shared_ptr<const onpdf::Bitmap>*>(info); }

}  // namespace

QImage to_qimage(onpdf::Bitmap&& bitmap, qreal device_pixel_ratio) {
  if (bitmap.empty()) return QImage();
  // QImage 의 cleanup 콜백이 Bitmap 을 소유한다 (QImageData 가 마지막으로 사라질 때 한 번 호출).
  // PDFium 출력은 불투명하므로 RGB32 로 두면 drawImage 가 블렌딩 없이 복사한다.
  onpdf::Bitmap* owner = std::make_unique<onpdf::Bitmap>(std::move(bitmap)).release();
  onpdf::Bitmap& b = *owner;
  QImage image(b.pixels.data(), b.width, b.height, b.stride, QImage::Format_RGB32, release_bitmap,
               owner);
  image.setDevicePixelRatio(device_pixel_ratio);
  return image;
}

QImage to_qimage_shared(std::shared_ptr<const onpdf::Bitmap> bitmap, qreal device_pixel_ratio) {
  if (!bitmap || bitmap->empty()) return QImage();
  // QImage 의 uchar* 생성자는 비const 버퍼를 요구하지만 RGB32 로 읽기만 하므로 const 를 벗긴다.
  // const 생성자를 쓰면 ro_data 가 되어 setDevicePixelRatio 가 깊은 복사를 한다.
  auto* owner = new std::shared_ptr<const onpdf::Bitmap>(std::move(bitmap));
  const onpdf::Bitmap& b = **owner;
  QImage image(const_cast<uchar*>(b.pixels.data()), b.width, b.height, b.stride, QImage::Format_RGB32,
               release_shared_bitmap, owner);
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
