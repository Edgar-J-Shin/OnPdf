#pragma once

#include "onpdf/core/bitmap.h"

#include <QImage>

namespace onpdf::app {

// BGRA Bitmap 을 QImage::Format_ARGB32 로 복사 없이 감싼다. 버퍼는 QImage(와 복사본들)가 살아 있는
// 동안 유지되고 마지막 참조가 사라질 때 해제된다. 반환된 이미지는 수정하지 않는다.
QImage to_qimage(onpdf::Bitmap&& bitmap, qreal device_pixel_ratio = 1.0);

// 테스트·비교용. 어떤 포맷의 QImage 든 BGRA Bitmap 으로 복사한다.
onpdf::Bitmap to_bitmap(const QImage& image);

}  // namespace onpdf::app
