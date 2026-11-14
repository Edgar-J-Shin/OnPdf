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
#include "onpdf/core/view/text_index.h"
#include "onpdf/core/view/text_selection.h"
#include "onpdf/core/view/tile_scheduler.h"
#include "ui_dispatcher.h"

#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QString>
#include <QTimer>

#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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
  // 쪽 포인트 좌표(좌상단 원점)가 뷰포트 상단에 오도록 스크롤 (링크 /XYZ 이동)
  void scroll_to_page_point(int page, onpdf::Point pt);
  [[nodiscard]] int current_page() const;  // 뷰포트 세로 중앙이 속한 쪽, 문서 없으면 -1
  [[nodiscard]] int page_count() const;

  // ---- 텍스트 선택·링크 ----
  [[nodiscard]] bool has_selection() const noexcept { return !selection_.empty(); }
  // 미로드 쪽은 동기 로드(상한 200쪽). 상한을 넘겨 잘렸으면 *truncated = true
  [[nodiscard]] QString selected_text(bool* truncated = nullptr) const;
  void select_all_on_current_page();
  void clear_selection();
  [[nodiscard]] const view::PageTextIndex* text_index(int page) const;  // 캐시에 있을 때만
  [[nodiscard]] const view::TextSelection& selection() const noexcept { return selection_; }
  // 테스트·도구용: 쪽 포인트 → 뷰포트 논리 좌표 (쪽이 뷰포트 밖이어도 계산)
  [[nodiscard]] QPointF page_point_to_viewport(int page, onpdf::Point pt) const;
  [[nodiscard]] std::optional<std::pair<int, onpdf::Point>> page_point_at(QPointF viewport_pos) const;

  [[nodiscard]] view::TileCache::Stats cache_stats() const { return cache_.stats(); }
  [[nodiscard]] std::vector<view::TileKey> cache_keys() const { return cache_.keys(); }
  [[nodiscard]] view::TileScheduler::Stats scheduler_stats() const;
  [[nodiscard]] int pending_visible_tiles() const;

  static constexpr double kMinZoom = 0.25;
  static constexpr double kMaxZoom = 8.0;

 signals:
  void current_page_changed(int page);
  void zoom_changed(double zoom);
  // 보이는 타일이 전부 도착한 최초 시점 (문서마다 한 번). 스크린샷·테스트용
  void first_view_rendered();
  void selection_changed(bool has_selection);
  void text_index_ready(int page);
  void link_activated(const onpdf::view::Link& link);   // 안전 검사 전. 수신자가 is_safe_uri 로 판단
  void link_hovered(const QString& target);             // 빈 문자열 = 호버 해제

 protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void scrollContentsBy(int dx, int dy) override;
  void wheelEvent(QWheelEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void leaveEvent(QEvent* event) override;
  bool viewportEvent(QEvent* event) override;  // 핀치(NativeGesture)는 viewport 로 온다
  bool event(QEvent* event) override;          // DPR 변경

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
  // 텍스트
  void request_text_indexes();
  void on_text_index(int page, int generation, std::shared_ptr<const view::PageTextIndex> index, bool cancelled);
  void paint_selection(QPainter& painter, int page, const QRectF& page_logical);
  [[nodiscard]] view::TextPos position_at(QPointF viewport_pos) const;  // 가장 가까운 선택 위치 (쪽 밖은 clamp)
  void update_hover(QPointF viewport_pos);
  void set_selection(view::TextPos anchor, view::TextPos focus);
  void autoscroll_step();

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

  // 텍스트 인덱스·선택 (메인 스레드 전용)
  engine::DocumentHandle handle_;
  UiDispatcher* dispatcher_ = nullptr;
  int doc_generation_ = 0;
  std::unordered_map<int, std::shared_ptr<const view::PageTextIndex>> text_cache_;
  std::unordered_map<int, engine::TaskHandle> text_requested_;  // 쪽 → 취소 핸들
  view::TextSelection selection_;
  enum class Drag { kNone, kText, kMaybeLink } drag_ = Drag::kNone;
  QPointF press_pos_;
  std::optional<view::Link> pressed_link_;
  QString hovered_target_;
  QTimer autoscroll_timer_;
  QPointF last_drag_pos_;
  QElapsedTimer click_timer_;
  int click_count_ = 0;
  QPointF last_click_pos_;
  static constexpr int kTextKeepPages = 5;
};

}  // namespace onpdf::app
