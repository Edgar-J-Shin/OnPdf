#include "page_view.h"

#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>

#include <algorithm>

namespace onpdf::app {

PageView::PageView(QWidget* parent) : QWidget(parent) {
  setAutoFillBackground(false);
  resize_timer_.setSingleShot(true);
  resize_timer_.setInterval(100);
  connect(&resize_timer_, &QTimer::timeout, this, &PageView::viewport_resized);
}

void PageView::set_page(const QImage& image, engine::PageSize size_pt) {
  image_ = image;
  size_pt_ = size_pt;
  placeholder_.clear();
  update();
}

void PageView::clear(const QString& placeholder) {
  image_ = QImage();
  size_pt_ = {};
  placeholder_ = placeholder;
  update();
}

float PageView::fit_width_scale(engine::PageSize size_pt) const {
  const double available = std::max(1, width() - 2 * kMargin);
  if (size_pt.width <= 0.0) return 1.0f;
  return static_cast<float>(available / size_pt.width);
}

void PageView::paintEvent(QPaintEvent* event) {
  QPainter painter(this);
  painter.fillRect(event->rect(), QColor(0x80, 0x80, 0x80));

  if (image_.isNull()) {
    painter.setPen(QColor(0xf0, 0xf0, 0xf0));
    painter.drawText(rect(), Qt::AlignCenter, placeholder_);
    return;
  }

  const QSizeF logical = image_.deviceIndependentSize();
  const QPointF top_left((width() - logical.width()) / 2.0,
                         std::max(static_cast<double>(kMargin), (height() - logical.height()) / 2.0));
  painter.drawImage(QRectF(top_left, logical), image_);
}

void PageView::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  resize_timer_.start();
}

}  // namespace onpdf::app
