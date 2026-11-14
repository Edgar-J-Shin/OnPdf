#include "document_view.h"

#include "document_tile_source.h"
#include "onpdf/core/view/tile.h"
#include "qt_image.h"

#include <QKeyEvent>
#include <QNativeGestureEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace onpdf::app {

namespace {
constexpr int kSingleStep = 40;
constexpr int kShadow = 2;
const QColor kBackground(0x80, 0x80, 0x80);
const QColor kShadowColor(0, 0, 0, 60);
}  // namespace

DocumentView::DocumentView(QWidget* parent) : QAbstractScrollArea(parent) {
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  verticalScrollBar()->setSingleStep(kSingleStep);
  horizontalScrollBar()->setSingleStep(kSingleStep);
  viewport()->setAutoFillBackground(false);
  setFocusPolicy(Qt::StrongFocus);
  resize_timer_.setSingleShot(true);
  resize_timer_.setInterval(100);
  connect(&resize_timer_, &QTimer::timeout, this, &DocumentView::relayout);
}

DocumentView::~DocumentView() = default;

// ---------------------------------------------------------------- 문서

void DocumentView::set_document(const engine::DocumentHandle& handle, UiDispatcher& dispatcher) {
  // 이전 스케줄러 소멸 → in-flight 취소, 늦은 콜백은 weak_ptr 로 무시
  scheduler_.reset();
  source_.reset();
  cache_.clear();
  layout_ = std::make_unique<view::PageLayout>(handle.info().page_sizes);
  source_ = std::make_unique<DocumentTileSource>(handle, dispatcher, this);
  scheduler_ = std::make_unique<view::TileScheduler>(*source_, cache_);
  scheduler_->set_layout(layout_.get());
  scheduler_->set_on_tile([this](const view::TileKey& key) { on_tile_ready(key); });
  placeholder_.clear();
  last_page_ = -1;
  first_view_pending_ = true;
  verticalScrollBar()->setValue(0);
  horizontalScrollBar()->setValue(0);
  relayout();
  emit_page_if_changed();
}

void DocumentView::clear_document(const QString& placeholder) {
  scheduler_.reset();
  source_.reset();
  layout_.reset();
  cache_.clear();
  placeholder_ = placeholder;
  last_page_ = -1;
  first_view_pending_ = false;
  update_scrollbars();
  viewport()->update();
}

// ---------------------------------------------------------------- 배율

void DocumentView::set_zoom_mode(ZoomMode mode) {
  mode_ = mode;
  relayout();
}

void DocumentView::set_zoom(double zoom) {
  set_zoom(zoom, QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0));
}

void DocumentView::set_zoom(double zoom, QPointF anchor_viewport) {
  zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
  mode_ = ZoomMode::kCustom;
  if (!layout_) {
    zoom_ = zoom;
    return;
  }
  // 앵커 아래 문서 지점을 (쪽, 쪽 내 비율) 로 기록
  const double d = dpr();
  const onpdf::Rect vp = viewport_doc();
  const onpdf::Point doc_before{vp.x + anchor_viewport.x() * d, vp.y + anchor_viewport.y() * d};
  const int page = layout_->page_at(doc_before.y);
  const onpdf::Rect r0 = layout_->page_rect(page);
  const double fx = (doc_before.x - r0.x) / std::max(1.0, r0.w);
  const double fy = (doc_before.y - r0.y) / std::max(1.0, r0.h);

  zoom_ = zoom;
  apply_scale_q(view::quantize_scale(zoom_ * d));

  const onpdf::Rect r1 = layout_->page_rect(page);
  const onpdf::Point doc_after{r1.x + fx * r1.w, r1.y + fy * r1.h};
  verticalScrollBar()->setValue(static_cast<int>(std::lround(doc_after.y / d - anchor_viewport.y())));
  horizontalScrollBar()->setValue(static_cast<int>(std::lround(doc_after.x / d - anchor_viewport.x())));
  schedule_update();
  viewport()->update();
  emit zoom_changed(zoom_);
}

void DocumentView::zoom_in() { set_zoom(zoom_ * 1.25); }
void DocumentView::zoom_out() { set_zoom(zoom_ / 1.25); }

// ---------------------------------------------------------------- 쪽 이동

void DocumentView::go_to_page(int page) {
  if (!layout_ || layout_->page_count() == 0) return;
  page = std::clamp(page, 0, layout_->page_count() - 1);
  const onpdf::Rect r = layout_->page_rect(page);
  verticalScrollBar()->setValue(static_cast<int>(std::lround((r.y - layout_->options().margin) / dpr())));
}

int DocumentView::current_page() const {
  if (!layout_ || layout_->page_count() == 0) return -1;
  const onpdf::Rect vp = viewport_doc();
  return layout_->page_at(vp.y + vp.h / 2.0);
}

int DocumentView::page_count() const { return layout_ ? layout_->page_count() : 0; }

view::TileScheduler::Stats DocumentView::scheduler_stats() const {
  return scheduler_ ? scheduler_->stats() : view::TileScheduler::Stats{};
}

int DocumentView::pending_visible_tiles() const {
  return scheduler_ ? scheduler_->pending_visible() : 0;
}

// ---------------------------------------------------------------- 레이아웃

double DocumentView::dpr() const { return viewport()->devicePixelRatioF(); }

onpdf::Rect DocumentView::viewport_doc() const {
  const double d = dpr();
  return {horizontalScrollBar()->value() * d, verticalScrollBar()->value() * d,
          viewport()->width() * d, viewport()->height() * d};
}

double DocumentView::horizontal_offset_logical() const {
  if (!layout_) return 0.0;
  const double doc_w = layout_->document_width() / dpr();
  return doc_w < viewport()->width() ? (viewport()->width() - doc_w) / 2.0 : 0.0;
}

void DocumentView::relayout() {
  if (!layout_) {
    update_scrollbars();
    viewport()->update();
    return;
  }
  const double d = dpr();
  int scale_q = layout_->scale_q();
  switch (mode_) {
    case ZoomMode::kFitWidth:
      scale_q = layout_->fit_width_scale_q(viewport()->width() * d);
      break;
    case ZoomMode::kFitPage: {
      const int page = std::max(0, current_page());
      scale_q = layout_->fit_page_scale_q(viewport()->width() * d, viewport()->height() * d, page);
      break;
    }
    case ZoomMode::kCustom:
      scale_q = view::quantize_scale(zoom_ * d);
      break;
  }
  const double min_q = kMinZoom * d * view::kScaleQuantum;
  const double max_q = kMaxZoom * d * view::kScaleQuantum;
  scale_q = static_cast<int>(std::clamp<double>(scale_q, min_q, max_q));
  zoom_ = view::dequantize_scale(scale_q) / d;

  // 현재 쪽의 상단을 유지하며 배율 적용
  const int page = std::max(0, current_page());
  const double top_fraction = [&] {
    const onpdf::Rect r = layout_->page_rect(page);
    return (viewport_doc().y - r.y) / std::max(1.0, r.h);
  }();
  apply_scale_q(scale_q);
  const onpdf::Rect r1 = layout_->page_rect(page);
  verticalScrollBar()->setValue(static_cast<int>(std::lround((r1.y + top_fraction * r1.h) / d)));
  schedule_update();
  viewport()->update();
  emit zoom_changed(zoom_);
}

void DocumentView::apply_scale_q(int scale_q) {
  if (layout_->scale_q() != scale_q) {
    layout_->set_scale_q(scale_q);
    scheduler_->set_layout(layout_.get());  // 실패 집합 초기화
  }
  update_scrollbars();
}

void DocumentView::update_scrollbars() {
  if (!layout_) {
    verticalScrollBar()->setRange(0, 0);
    horizontalScrollBar()->setRange(0, 0);
    return;
  }
  const double d = dpr();
  const int doc_h = static_cast<int>(std::ceil(layout_->total_height() / d));
  const int doc_w = static_cast<int>(std::ceil(layout_->document_width() / d));
  verticalScrollBar()->setRange(0, std::max(0, doc_h - viewport()->height()));
  verticalScrollBar()->setPageStep(viewport()->height());
  horizontalScrollBar()->setRange(0, std::max(0, doc_w - viewport()->width()));
  horizontalScrollBar()->setPageStep(viewport()->width());
}

void DocumentView::schedule_update() {
  if (!scheduler_) return;
  scheduler_->update(viewport_doc());
  if (first_view_pending_ && scheduler_->pending_visible() == 0) {
    first_view_pending_ = false;
    emit first_view_rendered();
  }
}

void DocumentView::on_tile_ready(const view::TileKey& key) {
  if (!layout_) return;
  const double d = dpr();
  const onpdf::Rect page = layout_->page_rect(key.page);
  // 어떤 배율의 타일이든 현재 배율 기준 쪽 사각형 전체를 다시 그리면 충분히 싸다(쪽은 뷰포트와 교차하는 것만)
  const QRectF logical(horizontal_offset_logical() + page.x / d - horizontalScrollBar()->value(),
                       page.y / d - verticalScrollBar()->value(), page.w / d, page.h / d);
  viewport()->update(logical.toAlignedRect().adjusted(-1, -1, kShadow + 1, kShadow + 1));
  if (first_view_pending_ && scheduler_ && scheduler_->pending_visible() == 0) {
    first_view_pending_ = false;
    emit first_view_rendered();
  }
}

void DocumentView::emit_page_if_changed() {
  const int page = current_page();
  if (page != last_page_) {
    last_page_ = page;
    emit current_page_changed(page);
  }
}

// ---------------------------------------------------------------- 그리기

void DocumentView::paintEvent(QPaintEvent* event) {
  QPainter painter(viewport());
  painter.fillRect(event->rect(), kBackground);
  if (!layout_) {
    painter.setPen(QColor(0xf0, 0xf0, 0xf0));
    painter.drawText(viewport()->rect(), Qt::AlignCenter, placeholder_);
    return;
  }
  const double d = dpr();
  const onpdf::Rect vp = viewport_doc();
  const double ox = horizontal_offset_logical() - horizontalScrollBar()->value();
  const double oy = -verticalScrollBar()->value();
  for (int page : layout_->pages_in(vp)) {
    const onpdf::Rect r = layout_->page_rect(page);
    const QRectF logical(ox + r.x / d, oy + r.y / d, r.w / d, r.h / d);
    if (!logical.adjusted(0, 0, kShadow, kShadow).intersects(QRectF(event->rect()))) continue;
    paint_page(painter, page, logical);
  }
}

void DocumentView::paint_page(QPainter& painter, int page, const QRectF& page_logical) {
  const double d = dpr();
  painter.fillRect(page_logical.translated(kShadow, kShadow), kShadowColor);
  painter.fillRect(page_logical, Qt::white);

  const int scale_q = layout_->scale_q();
  const engine::PageSize pt = layout_->page_size_pt(page);
  const double cur = view::dequantize_scale(scale_q);

  // 1. 대체 타일: 다른 배율(미리보기·이전 배율)을 현재 배율로 늘려 그린다. 작은 배율(흐린 것)부터
  auto others = cache_.tiles_for_page(page);
  std::sort(others.begin(), others.end(), [](const auto& a, const auto& b) { return a.first.scale_q < b.first.scale_q; });
  for (const auto& [key, bitmap] : others) {
    if (key.scale_q == scale_q) continue;
    const double factor = cur / view::dequantize_scale(key.scale_q);
    const onpdf::Rect src = view::tile_rect_px(key, pt);
    const QRectF target(page_logical.x() + src.x * factor / d, page_logical.y() + src.y * factor / d,
                        src.w * factor / d, src.h * factor / d);
    painter.drawImage(target, to_qimage(onpdf::Bitmap(*bitmap), 1.0));
  }

  // 2. 현재 배율 타일 (뷰포트와 교차하는 것만)
  const onpdf::Rect vp = viewport_doc();
  const onpdf::Rect pr = layout_->page_rect(page);
  const onpdf::Rect vis = vp.intersected(pr).translated(-pr.x, -pr.y);
  if (vis.empty()) return;
  const onpdf::Size px = view::page_pixel_size(pt, scale_q);
  const int tx0 = static_cast<int>(vis.x) / view::kTileSize;
  const int ty0 = static_cast<int>(vis.y) / view::kTileSize;
  const int tx1 = std::min(view::tiles_across(px.w) - 1, static_cast<int>(std::ceil(vis.right()) - 1) / view::kTileSize);
  const int ty1 = std::min(view::tiles_across(px.h) - 1, static_cast<int>(std::ceil(vis.bottom()) - 1) / view::kTileSize);
  for (int ty = ty0; ty <= ty1; ++ty) {
    for (int tx = tx0; tx <= tx1; ++tx) {
      const view::TileKey key{page, scale_q, tx, ty};
      auto bitmap = cache_.get(key);
      if (!bitmap) continue;
      const onpdf::Rect src = view::tile_rect_px(key, pt);
      QImage image = to_qimage(onpdf::Bitmap(*bitmap), d);
      painter.drawImage(QPointF(page_logical.x() + src.x / d, page_logical.y() + src.y / d), image);
    }
  }
}

// ---------------------------------------------------------------- 이벤트

void DocumentView::resizeEvent(QResizeEvent* event) {
  QAbstractScrollArea::resizeEvent(event);
  update_scrollbars();
  if (mode_ == ZoomMode::kCustom) {
    schedule_update();
  } else {
    resize_timer_.start();  // 맞춤 모드는 배율 재계산을 디바운스. 그 사이 기존 타일을 늘려 그린다
  }
}

void DocumentView::scrollContentsBy(int dx, int dy) {
  QAbstractScrollArea::scrollContentsBy(dx, dy);
  schedule_update();
  emit_page_if_changed();
  viewport()->update();
}

void DocumentView::wheelEvent(QWheelEvent* event) {
  if (event->modifiers() & Qt::ControlModifier) {
    const double steps = event->angleDelta().y() / 120.0;
    if (steps != 0.0) set_zoom(zoom_ * std::pow(1.1, steps), event->position());
    event->accept();
    return;
  }
  QAbstractScrollArea::wheelEvent(event);
}

void DocumentView::keyPressEvent(QKeyEvent* event) {
  QScrollBar* v = verticalScrollBar();
  switch (event->key()) {
    case Qt::Key_PageDown: v->setValue(v->value() + v->pageStep()); break;
    case Qt::Key_PageUp: v->setValue(v->value() - v->pageStep()); break;
    case Qt::Key_Home: v->setValue(v->minimum()); break;
    case Qt::Key_End: v->setValue(v->maximum()); break;
    default: QAbstractScrollArea::keyPressEvent(event); return;
  }
  event->accept();
}

bool DocumentView::event(QEvent* event) {
  if (event->type() == QEvent::NativeGesture) {
    auto* gesture = static_cast<QNativeGestureEvent*>(event);
    if (gesture->gestureType() == Qt::ZoomNativeGesture) {
      set_zoom(zoom_ * (1.0 + gesture->value()), gesture->position());
      return true;
    }
  }
  return QAbstractScrollArea::event(event);
}

}  // namespace onpdf::app
