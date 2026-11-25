#include "image_import.h"

#include <QFile>
#include <QImageReader>
#include <QMimeDatabase>
#include <QStringList>
#include <algorithm>
#include <cstring>

namespace onpdf::app {
namespace {

// Qt 의 PDF 이미지 플러그인(QtPdf)이 있으면 "pdf" 도 이미지 형식으로 보인다 — PDF 는 이미지로
// 다루지 않는다 (드롭은 열기·삽입)
bool is_excluded_format(const QString& format) {
  const QString f = format.toLower();
  return f == QStringLiteral("pdf") || f == QStringLiteral("svg") || f == QStringLiteral("svgz");
}

Result<ImportedImage> from_qimage(QImage image, const QString& format) {
  if (image.isNull()) return make_error(ErrorCode::kInvalidFormat, "empty image");
  if (static_cast<std::int64_t>(image.width()) * image.height() > engine::kMaxImagePixels) {
    return make_error(ErrorCode::kInvalidArgument, "image is too large");
  }
  // ARGB32 는 직선 알파, 리틀 엔디언 메모리 배치가 B G R A — PDFium 의 FPDFBitmap_BGRA 와 같다
  image = image.convertToFormat(QImage::Format_ARGB32);
  engine::ImagePixels pixels;
  pixels.width = image.width();
  pixels.height = image.height();
  pixels.bgra.resize(static_cast<std::size_t>(pixels.width) *
                     static_cast<std::size_t>(pixels.height) * 4);
  for (int y = 0; y < pixels.height; ++y) {
    std::memcpy(pixels.bgra.data() +
                    static_cast<std::size_t>(y) * static_cast<std::size_t>(pixels.width) * 4,
                image.constScanLine(y), static_cast<std::size_t>(pixels.width) * 4);
  }
  ImportedImage out;
  out.pixel_width = pixels.width;
  out.pixel_height = pixels.height;
  out.format = format;
  out.spec.source = std::move(pixels);
  return out;
}

}  // namespace

Result<ImportedImage> import_image_file(const QString& path, bool force_pixels) {
  QImageReader reader(path);
  reader.setAutoTransform(true);  // EXIF 회전을 픽셀에 반영
  const QString format = QString::fromLatin1(reader.format());
  if (format.isEmpty())
    return make_error(ErrorCode::kInvalidFormat, reader.errorString().toStdString());
  if (is_excluded_format(format))
    return make_error(ErrorCode::kInvalidFormat, "not a raster image");
  const QSize size = reader.size();
  if (size.isValid() &&
      static_cast<std::int64_t>(size.width()) * size.height() > engine::kMaxImagePixels) {
    return make_error(ErrorCode::kInvalidArgument, "image is too large");
  }
  const bool jpeg = format == QStringLiteral("jpeg") || format == QStringLiteral("jpg");
  if (jpeg && !force_pixels && reader.transformation() == QImageIOHandler::TransformationNone &&
      size.isValid()) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
      return make_error(ErrorCode::kIoError, file.errorString().toStdString());
    const QByteArray bytes = file.readAll();
    ImportedImage out;
    out.pixel_width = size.width();
    out.pixel_height = size.height();
    out.format = QStringLiteral("jpeg");
    out.spec.source = engine::JpegBytes{std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
                                        size.width(), size.height()};
    return out;
  }
  QImage image = reader.read();  // GIF·애니메이션은 첫 프레임
  if (image.isNull())
    return make_error(ErrorCode::kInvalidFormat, reader.errorString().toStdString());
  return from_qimage(std::move(image), format);
}

Result<ImportedImage> import_image(const QImage& image) {
  return from_qimage(image, QStringLiteral("png"));
}

Rect place_image(engine::PageSize page, Point origin, int pixel_width, int pixel_height,
                 bool center) {
  double w = std::max(1, pixel_width);
  double h = std::max(1, pixel_height);
  const double max_w = page.width * 0.8;
  const double max_h = page.height * 0.8;
  const double scale = std::min({1.0, max_w / w, max_h / h});
  w *= scale;
  h *= scale;
  double x = center ? origin.x - w / 2 : origin.x;
  double y = center ? origin.y - h / 2 : origin.y;
  x = std::clamp(x, 0.0, std::max(0.0, page.width - w));
  y = std::clamp(y, 0.0, std::max(0.0, page.height - h));
  return {x, y, w, h};
}

QString image_file_filter() {
  QStringList patterns;
  for (const QByteArray& f : QImageReader::supportedImageFormats()) {
    if (!is_excluded_format(QString::fromLatin1(f)))
      patterns << QStringLiteral("*.") + QString::fromLatin1(f);
  }
  patterns.removeDuplicates();
  return QObject::tr("Images (%1)").arg(patterns.join(QLatin1Char(' ')));
}

bool looks_like_image_file(const QString& path) {
  const QString suffix = path.section(QLatin1Char('.'), -1).toLower();
  if (is_excluded_format(suffix)) return false;
  for (const QByteArray& f : QImageReader::supportedImageFormats()) {
    if (suffix == QString::fromLatin1(f).toLower()) return true;
  }
  const QString mime = QMimeDatabase().mimeTypeForFile(path).name();
  return mime.startsWith(QStringLiteral("image/")) && !mime.contains(QStringLiteral("svg"));
}

}  // namespace onpdf::app
