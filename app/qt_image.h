#pragma once

#include "onpdf/core/bitmap.h"

#include <QImage>

#include <memory>

namespace onpdf::app {

// BGRA Bitmap 을 QImage::Format_RGB32 로 복사 없이 감싼다 (PDFium 출력은 불투명). 버퍼는 QImage(와 복사본들)가 살아 있는
// 동안 유지되고 마지막 참조가 사라질 때 해제된다. 반환된 이미지는 수정하지 않는다.
QImage to_qimage(onpdf::Bitmap&& bitmap, qreal device_pixel_ratio = 1.0);

// 캐시가 공유하는 비트맵을 복사 없이 감싼다. QImage(와 복사본)가 살아 있는 동안 shared_ptr 이 유지된다.
// 외부 버퍼라 수정하면 안 된다 (const). 그리기 전용.
QImage to_qimage_shared(std::shared_ptr<const onpdf::Bitmap> bitmap, qreal device_pixel_ratio = 1.0);

// 테스트·비교용. 어떤 포맷의 QImage 든 BGRA Bitmap 으로 복사한다.
onpdf::Bitmap to_bitmap(const QImage& image);

}  // namespace onpdf::app
