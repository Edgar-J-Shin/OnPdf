#pragma once

// 페이지 이미지 하나를 회색 배경 위에 중앙 정렬로 그리는 위젯.
// M1 에서 연속 스크롤·타일 캐시가 들어오면 교체된다.

#include "onpdf/core/engine/render_types.h"

#include <QImage>
#include <QString>
#include <QTimer>
#include <QWidget>

namespace onpdf::app {

class PageView : public QWidget {
  Q_OBJECT
 public:
  explicit PageView(QWidget* parent = nullptr);

  void set_page(const QImage& image, engine::PageSize size_pt);
  void clear(const QString& placeholder);

  // 페이지를 뷰 너비(여백 제외)에 맞추는 scale (포인트당 논리 픽셀). dpr 은 호출자가 곱한다.
  [[nodiscard]] float fit_width_scale(engine::PageSize size_pt) const;

  QSize sizeHint() const override { return {800, 900}; }

 signals:
  // resize 가 멈춘 뒤 100ms 후 한 번
  void viewport_resized();

 protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

 private:
  static constexpr int kMargin = 16;
  QImage image_;
  engine::PageSize size_pt_{};
  QString placeholder_;
  QTimer resize_timer_;
};

}  // namespace onpdf::app
