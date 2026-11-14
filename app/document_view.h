#pragma once

// 연속 스크롤 뷰. 모든 쪽을 세로로 이어 붙여 타일 단위로 그린다.
// 레이아웃·캐시·스케줄러(core/view)를 소유하고, 타일은 DocumentTileSource 를 통해 워커에서 렌더된다.
//
// 좌표계
//  - 논리 px: Qt 위젯·스크롤바 (dpr 제외)
//  - 문서 px: core::PageLayout (dpr 포함 물리 px). 변환: 문서 = 논리 × dpr
//  - 타일 배율 scale_q = quantize(zoom × dpr)

#include "onpdf/core/engine/document_handle.h"
#include "onpdf/core/view/page_layout.h"
#include "onpdf/core/view/tile_cache.h"
#include "onpdf/core/view/tile_scheduler.h"
#include "ui_dispatcher.h"

#include <QAbstractScrollArea>
#include <QImage>
#include <QPointF>
#include <QString>
#include <QTimer>

#include <memory>

namespace onpdf::app {

class DocumentTileSource;

enum class ZoomMode { kFitWidth, kFitPage, kCustom };

class DocumentView : public QAbstractScrollArea {
  Q_OBJECT
 public:
  explicit DocumentView(QWidget* parent = nullptr);
  ~DocumentView() override;

  void set_document(const engine::DocumentHandle& handle, UiDispatcher& dispatcher);
  void clear_document(const QString& placeholder);
  [[nodiscard]] bool has_document() const noexcept { return layout_ != nullptr; }

  void set_zoom_mode(ZoomMode mode);
  [[nodiscard]] ZoomMode zoom_mode() const noexcept { return mode_; }
  // 논리 배율(px/pt). anchor 는 뷰포트 논리 좌표; 그 아래 문서 지점이 고정된다
  void set_zoom(double zoom, QPointF anchor_viewport);
  void set_zoom(double zoom);  // 뷰포트 중앙 앵커
  void zoom_in();
  void zoom_out();
  [[nodiscard]] double zoom() const noexcept { return zoom_; }

  void go_to_page(int page);
  [[nodiscard]] int current_page() const;  // 뷰포트 세로 중앙이 속한 쪽, 문서 없으면 -1
  [[nodiscard]] int page_count() const;

  [[nodiscard]] view::TileCache::Stats cache_stats() const { return cache_.stats(); }
  [[nodiscard]] view::TileScheduler::Stats scheduler_stats() const;
  [[nodiscard]] int pending_visible_tiles() const;

  static constexpr double kMinZoom = 0.25;
  static constexpr double kMaxZoom = 8.0;

 signals:
  void current_page_changed(int page);
  void zoom_changed(double zoom);
  // 보이는 타일이 전부 도착한 최초 시점 (문서마다 한 번). 스크린샷·테스트용
  void first_view_rendered();

 protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void scrollContentsBy(int dx, int dy) override;
  void wheelEvent(QWheelEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  bool event(QEvent* event) override;

 private:
  [[nodiscard]] double dpr() const;
  [[nodiscard]] onpdf::Rect viewport_doc() const;      // 문서 px
  [[nodiscard]] double horizontal_offset_logical() const;  // 문서가 뷰보다 좁을 때 가운데 정렬 오프셋
  void relayout();                                       // 배율 계산 → 스크롤바 → 스케줄러
  void apply_scale_q(int scale_q);
  void update_scrollbars();
  void schedule_update();
  void paint_page(QPainter& painter, int page, const QRectF& page_logical);
  void on_tile_ready(const view::TileKey& key);
  void emit_page_if_changed();

  std::unique_ptr<view::PageLayout> layout_;
  view::TileCache cache_;
  std::unique_ptr<DocumentTileSource> source_;
  std::unique_ptr<view::TileScheduler> scheduler_;
  ZoomMode mode_ = ZoomMode::kFitWidth;
  double zoom_ = 1.0;
  int last_page_ = -1;
  bool first_view_pending_ = false;
  QString placeholder_;
  QTimer resize_timer_;
};

}  // namespace onpdf::app
