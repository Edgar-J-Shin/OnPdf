#pragma once

// 이미지 파일·클립보드 → engine::ImageSpec (#49). Qt 디코딩은 앱에만 있다 (core 는 픽셀·JPEG
// 바이트만 받는다). JPEG 은 EXIF 회전이 없으면 원본 바이트 그대로(재압축 없음), 그 밖은 QImage →
// 직선 알파 BGRA.

#include "onpdf/core/engine/page_object.h"
#include "onpdf/core/engine/render_types.h"
#include "onpdf/core/error.h"
#include "onpdf/core/geometry.h"

#include <QImage>
#include <QString>

namespace onpdf::app {

struct ImportedImage {
  engine::ImageSpec spec;  // rect 는 비어 있다 — place_image 로 채운다
  int pixel_width = 0;
  int pixel_height = 0;
  QString format;  // "jpeg", "png" … (상태 표시)
};

// 파일. force_pixels 면 JPEG 도 디코딩해 픽셀로 (PDFium 이 그 JPEG 를 못 넣을 때 재시도)
[[nodiscard]] Result<ImportedImage> import_image_file(const QString& path,
                                                      bool force_pixels = false);
// 클립보드 등 메모리 이미지
[[nodiscard]] Result<ImportedImage> import_image(const QImage& image);

// 놓을 사각형 (쪽 포인트): 1px = 1pt, 쪽 폭의 80% 를 넘으면 비율 유지로 줄인다. center 면 origin 을
// 가운데로, 아니면 왼쪽 위로. 쪽 밖으로 나가지 않게 옮긴다
[[nodiscard]] Rect place_image(engine::PageSize page, Point origin, int pixel_width,
                               int pixel_height, bool center);

// QFileDialog 필터 ("이미지 (*.png *.jpg …)")
[[nodiscard]] QString image_file_filter();
// 확장자·MIME 로 이미지 파일인지 (드롭 분기)
[[nodiscard]] bool looks_like_image_file(const QString& path);

}  // namespace onpdf::app
