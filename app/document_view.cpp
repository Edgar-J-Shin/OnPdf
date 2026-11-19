#include "document_view.h"

#include "document_tile_source.h"
#include "onpdf/core/view/tile.h"
#include "qt_image.h"

#include <QApplication>
#include <QToolTip>
#include <QPolygonF>
#include <QPen>
#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

namespace onpdf::app {

namespace {
constexpr int kSingleStep = 40;
constexpr int kShadow = 2;
const QColor kBackground(0x80, 0x80, 0x80);
const QColor kShadowColor(0, 0, 0, 60);
const QColor kSelectionColor(0, 120, 215, 90);
constexpr QColor kAnnotationOutline(0, 120, 215, 255);  // 선택한 주석 테두리
constexpr QColor kInkPreview(200, 0, 120, 200);          // 그리는 중인 펜
constexpr double kMoveThresholdPt = 4.0;                 // 이보다 짧은 드래그는 이동이 아니다
const QColor kSearchHitColor(255, 220, 0, 110);
const QColor kSearchCurrentColor(255, 140, 0, 150);
constexpr double kLinkClickSlop = 4.0;      // 이 거리(논리 px) 미만 이동이면 클릭
constexpr int kMultiClickMs = 500;
constexpr int kAutoscrollMs = 30;
constexpr int kMaxCopyPages = 200;
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
  autoscroll_timer_.setInterval(kAutoscrollMs);
  connect(&autoscroll_timer_, &QTimer::timeout, this, &DocumentView::autoscroll_step);
  viewport()->setMouseTracking(true);
}

DocumentView::~DocumentView() = default;

// ---------------------------------------------------------------- 문서

void DocumentView::set_document(const engine::DocumentHandle& handle, UiDispatcher& dispatcher) {
  // 이전 스케줄러 소멸 → in-flight 취소, 늦은 콜백은 weak_ptr 로 무시
  scheduler_.reset();
  source_.reset();
  cache_.clear();
  ++doc_generation_;
  handle_ = handle;
  dispatcher_ = &dispatcher;
  text_cache_.clear();
  for (auto& [page, h] : text_requested_) h.cancel();
  text_requested_.clear();
  annot_cache_.clear();
  for (auto& [page, h] : annot_requested_) h.cancel();
  annot_requested_.clear();
  selected_annot_.reset();
  reselect_page_ = -1;
  reselect_id_.clear();
  text_box_cache_.clear();
  for (auto& [page, h] : text_box_requested_) h.cancel();
  text_box_requested_.clear();
  selected_text_.reset();
  reselect_text_page_ = -1;
  reselect_text_box_.clear();
  ink_points_.clear();
  ink_page_ = -1;
  clear_selection();
  pressed_link_.reset();
  drag_ = Drag::kNone;
  // 스크롤바는 스케줄러를 만들기 전에 초기화한다 (setValue 가 scrollContentsBy → 요청을 일으키므로)
  verticalScrollBar()->setValue(0);
  horizontalScrollBar()->setValue(0);
  layout_ = std::make_unique<view::PageLayout>(handle.info().page_sizes);
  source_ = std::make_unique<DocumentTileSource>(handle, dispatcher, this);
  scheduler_ = std::make_unique<view::TileScheduler>(*source_, cache_);
  scheduler_->set_layout(layout_.get());
  scheduler_->set_on_tile([this](const view::TileKey& key) { on_tile_ready(key); });
  placeholder_.clear();
  last_page_ = -1;
  nav_anchor_.reset();
  first_view_pending_ = true;
  relayout();
  emit_page_if_changed();
}

void DocumentView::invalidate_pages(const std::vector<int>& pages, bool sizes_changed) {
  if (!layout_ || !source_) return;
  // 진행 중인 텍스트 추출은 이전 모양 기준이다: 세대를 올려 늦은 결과를 버리고, 보이는 쪽은 다시 요청한다
  ++doc_generation_;
  for (auto& [page, task] : text_requested_) task.cancel();
  text_requested_.clear();
  bool selection_hit = false;
  for (int page : pages) {
    cache_.remove_page(page);
    text_cache_.erase(page);
    annot_cache_.erase(page);
    if (auto it = annot_requested_.find(page); it != annot_requested_.end()) {
      it->second.cancel();
      annot_requested_.erase(it);
    }
    text_box_cache_.erase(page);
    if (auto it = text_box_requested_.find(page); it != text_box_requested_.end()) {
      it->second.cancel();
      text_box_requested_.erase(it);
    }
    if (selected_text_ && selected_text_->page == page) {  // 편집 뒤 같은 상자를 조용히 다시 고른다
      reselect_text_page_ = page;
      reselect_text_box_ = selected_text_->box;
      selected_text_.reset();
    }
    if (selected_annot_ && selected_annot_->page == page) {
      // NM 이 있는 주석은 새 목록이 오면 조용히 다시 고른다 (두께 → 불투명도처럼 연달아 바꿀 수 있게; 안내 문구를 지우지 않게
      // 해제 신호는 내지 않는다). "#index" 는 밀릴 수 있어 해제
      const std::string id = selected_annot_->info.id;
      if (!id.empty() && id.front() != '#') {
        selected_annot_.reset();
        reselect_page_ = page;
        reselect_id_ = id;
      } else {
        clear_annotation_selection();
      }
    }
    if (!selection_.empty() && page >= selection_.start().page && page <= selection_.end().page) selection_hit = true;
  }
  if (selection_hit) clear_selection();
  pending_anchor_.reset();
  pending_focus_.reset();
  nav_anchor_.reset();
  // 이전 스케줄러를 버리면 대기 요청은 취소되고, 이미 렌더 중인 결과는 weak_ptr 로 무시된다
  scheduler_ = std::make_unique<view::TileScheduler>(*source_, cache_);
  scheduler_->set_on_tile([this](const view::TileKey& key) { on_tile_ready(key); });
  if (sizes_changed) {
    // 보던 쪽과 그 안의 위치(비율)를 기억해 새 레이아웃에서 복원한다
    const int page = std::max(0, current_page());
    const onpdf::Rect old_rect = layout_->page_rect(page);
    const double fraction = (viewport_doc().y - old_rect.y) / std::max(1.0, old_rect.h);
    const int scale_q = layout_->scale_q();
    layout_ = std::make_unique<view::PageLayout>(handle_.info().page_sizes);
    layout_->set_scale_q(scale_q);
    scheduler_->set_layout(layout_.get());
    relayout();  // 맞춤 배율 재계산 (가장 넓은 쪽이 바뀌었을 수 있다)
    const onpdf::Rect rect = layout_->page_rect(page);
    verticalScrollBar()->setValue(static_cast<int>(std::lround((rect.y + fraction * rect.h) / dpr())));
  } else {
    scheduler_->set_layout(layout_.get());
  }
  schedule_update();
  request_text_indexes();  // 보이는 쪽 ±1 의 주석 목록도 다시 요청한다
  // 재선택은 지금 요청된 쪽에서만: 보이지 않는 쪽이면 한참 뒤 스크롤할 때 갑자기 선택이 생기지 않게 포기한다
  if (reselect_page_ >= 0 && !annot_requested_.contains(reselect_page_)) {
    reselect_page_ = -1;
    reselect_id_.clear();
    emit annotation_selected(false, engine::AnnotationInfo{});
  }
  if (reselect_text_page_ >= 0 && !text_box_requested_.contains(reselect_text_page_)) {
    reselect_text_page_ = -1;
    reselect_text_box_.clear();
    emit text_box_selected(false, SelectedTextBox{});
  }
  viewport()->update();
}

void DocumentView::rebuild_pages(int keep_page) {
  if (!layout_ || !source_) return;
  ++doc_generation_;  // 진행 중인 텍스트 추출·타일은 옛 쪽 번호 기준이다
  for (auto& [page, task] : text_requested_) task.cancel();
  text_requested_.clear();
  text_cache_.clear();
  cache_.clear();
  annot_cache_.clear();
  for (auto& [page, task] : annot_requested_) task.cancel();
  annot_requested_.clear();
  clear_annotation_selection();
  ink_points_.clear();
  ink_page_ = -1;
  clear_selection();
  pending_anchor_.reset();
  pending_focus_.reset();
  nav_anchor_.reset();
  pressed_link_.reset();
  drag_ = Drag::kNone;
  const int scale_q = layout_->scale_q();
  layout_ = std::make_unique<view::PageLayout>(handle_.info().page_sizes);
  layout_->set_scale_q(scale_q);
  scheduler_ = std::make_unique<view::TileScheduler>(*source_, cache_);
  scheduler_->set_on_tile([this](const view::TileKey& key) { on_tile_ready(key); });
  scheduler_->set_layout(layout_.get());
  relayout();
  const int count = layout_->page_count();
  if (count > 0) go_to_page(std::clamp(keep_page, 0, count - 1));
  last_page_ = -1;  // 같은 번호라도 다른 쪽일 수 있다: 쪽 번호·썸네일 강조를 다시 맞춘다
  emit_page_if_changed();
  schedule_update();
  request_text_indexes();
  viewport()->update();
}

void DocumentView::clear_document(const QString& placeholder) {
  scheduler_.reset();
  source_.reset();
  annot_cache_.clear();
  for (auto& [page, h] : annot_requested_) h.cancel();
  annot_requested_.clear();
  selected_annot_.reset();
  reselect_page_ = -1;
  reselect_id_.clear();
  text_box_cache_.clear();
  for (auto& [page, h] : text_box_requested_) h.cancel();
  text_box_requested_.clear();
  selected_text_.reset();
  reselect_text_page_ = -1;
  reselect_text_box_.clear();
  ink_points_.clear();
  ink_page_ = -1;
  layout_.reset();
  cache_.clear();
  ++doc_generation_;
  handle_ = engine::DocumentHandle{};
  text_cache_.clear();
  for (auto& [page, h] : text_requested_) h.cancel();
  text_requested_.clear();
  clear_selection();
  pressed_link_.reset();
  drag_ = Drag::kNone;
  autoscroll_timer_.stop();
  placeholder_ = placeholder;
  last_page_ = -1;
  nav_anchor_.reset();
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
  // 문서가 뷰보다 좁으면 가운데 정렬 오프셋만큼 뷰 x 와 문서 x 가 어긋난다
  const double hoff_before = horizontal_offset_logical();
  const onpdf::Point doc_before{vp.x + (anchor_viewport.x() - hoff_before) * d,
                                vp.y + anchor_viewport.y() * d};
  const int page = layout_->page_at(doc_before.y);
  const onpdf::Rect r0 = layout_->page_rect(page);
  const double fx = (doc_before.x - r0.x) / std::max(1.0, r0.w);
  const double fy = (doc_before.y - r0.y) / std::max(1.0, r0.h);

  zoom_ = zoom;
  apply_scale_q(view::quantize_scale(zoom_ * d));

  const onpdf::Rect r1 = layout_->page_rect(page);
  const onpdf::Point doc_after{r1.x + fx * r1.w, r1.y + fy * r1.h};
  verticalScrollBar()->setValue(static_cast<int>(std::lround(doc_after.y / d - anchor_viewport.y())));
  horizontalScrollBar()->setValue(
      static_cast<int>(std::lround(doc_after.x / d - (anchor_viewport.x() - horizontal_offset_logical()))));
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
  scroll_to_anchored(page, (r.y - layout_->options().margin) / dpr());
}

void DocumentView::scroll_to_anchored(int page, double value_logical) {
  QScrollBar* bar = verticalScrollBar();
  const int value = std::clamp(static_cast<int>(std::lround(value_logical)), bar->minimum(), bar->maximum());
  nav_anchor_ = NavAnchor{page, value};  // 스크롤 전에 둬야 이번 이동의 쪽 변경 신호가 이 쪽을 알린다
  if (bar->value() != value) bar->setValue(value);  // → scrollContentsBy → emit_page_if_changed
  emit_page_if_changed();
}

int DocumentView::current_page() const {
  if (!layout_ || layout_->page_count() == 0) return -1;
  if (nav_anchor_ && nav_anchor_->scroll == verticalScrollBar()->value() && nav_anchor_->page < layout_->page_count()) {
    return nav_anchor_->page;
  }
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
  return doc_w < viewport()->width() ? std::floor((viewport()->width() - doc_w) / 2.0) : 0.0;
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
    // 배율이 바뀌면 같은 스크롤 값이라도 보이는 내용이 달라진다 → 이동 고정 쪽 해제
    nav_anchor_.reset();
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
  request_text_indexes();
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
  emit tiles_changed();
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

  // 뷰포트와 교차하는 현재 배율 타일 범위 (쪽 픽셀 좌표)
  const onpdf::Rect vp = viewport_doc();
  const onpdf::Rect pr = layout_->page_rect(page);
  const onpdf::Rect vis = vp.intersected(pr).translated(-pr.x, -pr.y);
  if (vis.empty()) return;
  const onpdf::Size px = view::page_pixel_size(pt, scale_q);
  const int tx0 = static_cast<int>(vis.x) / view::kTileSize;
  const int ty0 = static_cast<int>(vis.y) / view::kTileSize;
  const int tx1 = std::min(view::tiles_across(px.w) - 1, static_cast<int>(std::ceil(vis.right()) - 1) / view::kTileSize);
  const int ty1 = std::min(view::tiles_across(px.h) - 1, static_cast<int>(std::ceil(vis.bottom()) - 1) / view::kTileSize);

  // 1. 현재 배율 타일을 모으고, 빠진 것이 있는지 본다
  struct Ready { view::TileKey key; std::shared_ptr<const onpdf::Bitmap> bitmap; };
  std::vector<Ready> ready;
  bool missing = false;
  for (int ty = ty0; ty <= ty1; ++ty) {
    for (int tx = tx0; tx <= tx1; ++tx) {
      const view::TileKey key{page, scale_q, tx, ty};
      if (auto bitmap = cache_.get(key)) {
        ready.push_back({key, std::move(bitmap)});
      } else {
        missing = true;
      }
    }
  }

  // 2. 빠진 타일이 있을 때만 대체 타일(미리보기·이전 배율)을 뷰포트와 교차하는 것만 늘려 그린다.
  //    흐린 것(작은 배율)부터 그려 선명한 것이 위에 오게 한다
  if (missing) {
    auto others = cache_.tiles_for_page(page);
    std::sort(others.begin(), others.end(),
              [](const auto& a, const auto& b) { return a.first.scale_q < b.first.scale_q; });
    for (const auto& [key, bitmap] : others) {
      if (key.scale_q == scale_q) continue;
      const double factor = cur / view::dequantize_scale(key.scale_q);
      const onpdf::Rect src = view::tile_rect_px(key, pt);
      const onpdf::Rect in_cur{src.x * factor, src.y * factor, src.w * factor, src.h * factor};
      if (!in_cur.intersects(vis)) continue;
      const QRectF target(page_logical.x() + in_cur.x / d, page_logical.y() + in_cur.y / d, in_cur.w / d, in_cur.h / d);
      painter.drawImage(target, to_qimage_shared(bitmap, 1.0));  // 복사 없음
    }
  }

  // 3. 현재 배율 타일
  for (const auto& [key, bitmap] : ready) {
    const onpdf::Rect src = view::tile_rect_px(key, pt);
    painter.drawImage(QPointF(page_logical.x() + src.x / d, page_logical.y() + src.y / d),
                      to_qimage_shared(bitmap, d));  // 복사 없음
  }

  // 4. 검색 결과 (선택 아래)
  paint_search(painter, page, page_logical);
  // 5. 선택 하이라이트
  paint_selection(painter, page, page_logical);
  paint_annotations(painter, page, page_logical);
}

// ---------------------------------------------------------------- 텍스트 인덱스

void DocumentView::request_text_indexes() {
  if (!layout_ || !handle_.valid() || dispatcher_ == nullptr) return;
  const std::vector<int> visible = layout_->pages_in(viewport_doc());
  if (visible.empty()) return;
  const int first = std::max(0, visible.front() - 1);
  const int last = std::min(layout_->page_count() - 1, visible.back() + 1);
  const int center = current_page();
  // 유지 반경은 가시 범위보다 넓어야 저배율에서 해제·재요청이 반복되지 않는다
  const int keep = std::max(kTextKeepPages, last - first + 1);

  // 범위를 벗어난 대기 요청은 취소 (지나간 쪽을 전부 추출하지 않도록)
  for (auto it = text_requested_.begin(); it != text_requested_.end();) {
    if (it->first < first || it->first > last) {
      it->second.cancel();
      it = text_requested_.erase(it);
    } else {
      ++it;
    }
  }
  // 멀어진 쪽의 인덱스는 해제. 선택에 포함된 쪽은 유지 (링크 포인터는 복사본이라 안전)
  for (auto it = text_cache_.begin(); it != text_cache_.end();) {
    const bool far = std::abs(it->first - center) > keep;
    if (far && !selection_.contains_page(it->first)) {
      it = text_cache_.erase(it);
    } else {
      ++it;
    }
  }

  for (int page = first; page <= last; ++page) {
    if (text_cache_.contains(page) || text_requested_.contains(page)) continue;
    UiDispatcher* dispatcher = dispatcher_;
    QPointer<DocumentView> self(this);
    const int generation = doc_generation_;
    engine::TaskHandle handle = handle_.load_text(
        page, engine::Priority::kLow,
        // 워커 스레드: 메인으로 넘기기만
        [dispatcher, self, page, generation](onpdf::Result<view::PageTextIndex> result) {
          std::shared_ptr<const view::PageTextIndex> index;
          // 취소·문서 닫힘(kInvalidState) 은 일시적 — 캐시하지 않는다. 추출 실패만 빈 인덱스로 캐시
          const bool cancelled = !result && (result.error().code == onpdf::ErrorCode::kCancelled ||
                                             result.error().code == onpdf::ErrorCode::kInvalidState);
          if (result) {
            index = std::make_shared<const view::PageTextIndex>(std::move(*result));
          } else if (!cancelled) {
            std::fprintf(stderr, "[onpdf] text index failed for page %d: %s\n", page, result.error().message.c_str());
          }
          dispatcher->post_to(self, [page, generation, index, cancelled](DocumentView* v) {
            v->on_text_index(page, generation, cancelled ? nullptr : index, cancelled);
          });
        });
    text_requested_.emplace(page, std::move(handle));
  }
  request_annotations();
  request_text_boxes();
}

void DocumentView::request_annotations() {
  if (!layout_ || !handle_.valid() || dispatcher_ == nullptr) return;
  const std::vector<int> visible = layout_->pages_in(viewport_doc());
  if (visible.empty()) return;
  const int first = std::max(0, visible.front() - 1);
  const int last = std::min(layout_->page_count() - 1, visible.back() + 1);
  const int keep = std::max(kTextKeepPages, last - first + 1);
  const int center = current_page();
  for (auto it = annot_requested_.begin(); it != annot_requested_.end();) {
    if (it->first < first || it->first > last) {
      it->second.cancel();
      it = annot_requested_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = annot_cache_.begin(); it != annot_cache_.end();) {
    const bool far = std::abs(it->first - center) > keep;
    const bool selected = selected_annot_ && selected_annot_->page == it->first;
    if (far && !selected) {
      it = annot_cache_.erase(it);
    } else {
      ++it;
    }
  }
  for (int page = first; page <= last; ++page) {
    if (annot_cache_.contains(page) || annot_requested_.contains(page)) continue;
    UiDispatcher* dispatcher = dispatcher_;
    QPointer<DocumentView> self(this);
    const int generation = doc_generation_;
    engine::TaskHandle handle = handle_.load_annotations(
        page, engine::Priority::kLow,
        // 워커 스레드: 메인으로 넘기기만
        [dispatcher, self, page, generation](onpdf::Result<std::vector<engine::AnnotationInfo>> result) {
          std::shared_ptr<const std::vector<engine::AnnotationInfo>> list;
          const bool cancelled = !result && (result.error().code == onpdf::ErrorCode::kCancelled ||
                                             result.error().code == onpdf::ErrorCode::kInvalidState);
          if (result) list = std::make_shared<const std::vector<engine::AnnotationInfo>>(std::move(*result));
          dispatcher->post_to(self, [page, generation, list, cancelled](DocumentView* view) {
            view->on_annotations(page, generation, list, cancelled);
          });
        });
    annot_requested_[page] = std::move(handle);
  }
}

void DocumentView::on_annotations(int page, int generation, std::shared_ptr<const std::vector<engine::AnnotationInfo>> list,
                                  bool cancelled) {
  if (generation != doc_generation_) return;
  annot_requested_.erase(page);
  if (cancelled) return;
  if (!list) list = std::make_shared<const std::vector<engine::AnnotationInfo>>();  // 실패: 빈 목록으로 재요청 방지
  annot_cache_[page] = std::move(list);
  if (reselect_page_ == page) {
    reselect_page_ = -1;
    const std::string id = std::move(reselect_id_);
    reselect_id_.clear();
    if (!selected_annot_ && tool_ == AnnotationTool::kSelect) {
      for (const engine::AnnotationInfo& a : *annot_cache_[page]) {
        if (a.id != id || !a.editable) continue;
        selected_annot_ = SelectedAnnotation{page, a};
        update_page(page);
        break;
      }
    }
    if (selected_annot_ && selected_annot_->page == page && selected_annot_->info.id == id) {
      emit annotation_reselected(selected_annot_->info);
    } else {
      emit annotation_selected(false, engine::AnnotationInfo{});  // 지워졌거나 못 찾음: 버튼·메뉴 상태를 맞춘다
    }
  }
}

const std::vector<engine::AnnotationInfo>* DocumentView::annotations(int page) const {
  auto it = annot_cache_.find(page);
  return it == annot_cache_.end() ? nullptr : it->second.get();
}

// ---------------------------------------------------------------- 텍스트 상자 (#47)

void DocumentView::request_text_boxes() {
  if (!layout_ || !handle_.valid() || dispatcher_ == nullptr) return;
  const std::vector<int> visible = layout_->pages_in(viewport_doc());
  if (visible.empty()) return;
  const int first = std::max(0, visible.front() - 1);
  const int last = std::min(layout_->page_count() - 1, visible.back() + 1);
  const int keep = std::max(kTextKeepPages, last - first + 1);
  const int center = current_page();
  for (auto it = text_box_requested_.begin(); it != text_box_requested_.end();) {
    if (it->first < first || it->first > last) {
      it->second.cancel();
      it = text_box_requested_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = text_box_cache_.begin(); it != text_box_cache_.end();) {
    const bool far = std::abs(it->first - center) > keep;
    const bool selected = selected_text_ && selected_text_->page == it->first;
    if (far && !selected) {
      it = text_box_cache_.erase(it);
    } else {
      ++it;
    }
  }
  for (int page = first; page <= last; ++page) {
    if (text_box_cache_.contains(page) || text_box_requested_.contains(page)) continue;
    UiDispatcher* dispatcher = dispatcher_;
    QPointer<DocumentView> self(this);
    const int generation = doc_generation_;
    engine::TaskHandle handle = handle_.load_text_objects(
        page, engine::Priority::kLow,
        // 워커 스레드: 메인으로 넘기기만
        [dispatcher, self, page, generation](onpdf::Result<std::vector<engine::TextObjectInfo>> result) {
          std::shared_ptr<const std::vector<engine::TextObjectInfo>> list;
          const bool cancelled = !result && (result.error().code == onpdf::ErrorCode::kCancelled ||
                                             result.error().code == onpdf::ErrorCode::kInvalidState);
          if (result) list = std::make_shared<const std::vector<engine::TextObjectInfo>>(std::move(*result));
          dispatcher->post_to(self, [page, generation, list, cancelled](DocumentView* view) {
            view->on_text_boxes(page, generation, list, cancelled);
          });
        });
    text_box_requested_[page] = std::move(handle);
  }
}

void DocumentView::on_text_boxes(int page, int generation, std::shared_ptr<const std::vector<engine::TextObjectInfo>> list,
                                 bool cancelled) {
  if (generation != doc_generation_) return;
  text_box_requested_.erase(page);
  if (cancelled) return;
  if (!list) list = std::make_shared<const std::vector<engine::TextObjectInfo>>();
  text_box_cache_[page] = std::move(list);
  if (reselect_text_page_ == page) {
    reselect_text_page_ = -1;
    const std::string box = std::move(reselect_text_box_);
    reselect_text_box_.clear();
    if (!selected_text_ && tool_ == AnnotationTool::kSelect) {
      if (auto found = text_box_by_id(page, box)) {
        selected_text_ = found;
        update_page(page);
        emit text_box_reselected(*selected_text_);
        return;
      }
    }
    emit text_box_selected(false, SelectedTextBox{});  // 지워졌거나 못 찾음
  }
}

const std::vector<engine::TextObjectInfo>* DocumentView::text_objects(int page) const {
  auto it = text_box_cache_.find(page);
  return it == text_box_cache_.end() ? nullptr : it->second.get();
}

std::optional<DocumentView::SelectedTextBox> DocumentView::text_box_by_id(int page, const std::string& box) const {
  const auto* list = text_objects(page);
  if (list == nullptr) return std::nullopt;
  SelectedTextBox out;
  out.page = page;
  out.box = box;
  bool first = true;
  for (const engine::TextObjectInfo& t : *list) {
    if (t.box != box) continue;
    out.ids.push_back(t.id);
    if (first) {
      out.bbox = t.bbox;
      out.font_size = t.font_size;
      out.color = t.color;
      out.text = t.text;
      first = false;
    } else {
      const double x0 = std::min(out.bbox.x, t.bbox.x), y0 = std::min(out.bbox.y, t.bbox.y);
      const double x1 = std::max(out.bbox.right(), t.bbox.right()), y1 = std::max(out.bbox.bottom(), t.bbox.bottom());
      out.bbox = {x0, y0, x1 - x0, y1 - y0};
      out.text += "\n" + t.text;
    }
    ++out.lines;
  }
  if (out.ids.empty()) return std::nullopt;
  return out;
}

std::optional<DocumentView::SelectedTextBox> DocumentView::text_box_at(int page, onpdf::Point pt) const {
  const auto* list = text_objects(page);
  if (list == nullptr) return std::nullopt;
  for (auto it = list->rbegin(); it != list->rend(); ++it) {  // 뒤에 있는 것이 위에 그려진다
    if (it->bbox.contains(pt)) return text_box_by_id(page, it->box);
  }
  return std::nullopt;
}

void DocumentView::clear_text_box_selection() {
  const bool pending = reselect_text_page_ >= 0;
  reselect_text_page_ = -1;
  reselect_text_box_.clear();
  if (drag_ == Drag::kMoveText) cancel_text_drag();
  if (!selected_text_ && !pending) return;
  const int page = selected_text_ ? selected_text_->page : -1;
  selected_text_.reset();
  emit text_box_selected(false, SelectedTextBox{});
  if (page >= 0) update_page(page);
}

void DocumentView::cancel_text_drag() {
  if (drag_ == Drag::kMoveText) drag_ = Drag::kNone;
  const int page = selected_text_ ? selected_text_->page : -1;
  move_delta_ = {};
  if (page >= 0) update_page(page);
}

const engine::AnnotationInfo* DocumentView::annotation_at(int page, onpdf::Point pt) const {
  const auto* list = annotations(page);
  if (list == nullptr) return nullptr;
  for (auto it = list->rbegin(); it != list->rend(); ++it) {  // 뒤에 있는 것이 위에 그려진다
    if (it->flags & (engine::kAnnotFlagHidden | engine::kAnnotFlagNoView)) continue;  // 화면에 없는 주석은 고를 수 없다
    if (it->editable && it->rect.contains(pt)) return &*it;
  }
  return nullptr;
}

onpdf::Point DocumentView::clamp_to_page(int page, onpdf::Point pt) const {
  const engine::PageSize size = layout_->page_size_pt(page);
  return {std::clamp(pt.x, 0.0, std::max(0.0, size.width)), std::clamp(pt.y, 0.0, std::max(0.0, size.height))};
}

void DocumentView::update_page(int page) {
  if (!layout_ || page < 0 || page >= layout_->page_count()) return;
  const double d = dpr();
  const onpdf::Rect r = layout_->page_rect(page);
  viewport()->update(QRectF(horizontal_offset_logical() + r.x / d - horizontalScrollBar()->value(),
                            r.y / d - verticalScrollBar()->value(), r.w / d, r.h / d)
                         .toAlignedRect()
                         .adjusted(-2, -2, 2, 2));
}

void DocumentView::set_tool(AnnotationTool tool) {
  if (tool_ == tool) return;
  tool_ = tool;
  if (tool_ != AnnotationTool::kSelect) {
    clear_annotation_selection();
    clear_text_box_selection();
  }
  if (drag_ == Drag::kInk) {
    drag_ = Drag::kNone;
    ink_points_.clear();
    ink_page_ = -1;
  }
  cancel_shape_drag();
  update_hover(viewport()->mapFromGlobal(QCursor::pos()));
  viewport()->update();
}

onpdf::Rect DocumentView::shape_rect() const noexcept {
  const double x0 = std::min(shape_anchor_.x, shape_corner_.x), y0 = std::min(shape_anchor_.y, shape_corner_.y);
  const double x1 = std::max(shape_anchor_.x, shape_corner_.x), y1 = std::max(shape_anchor_.y, shape_corner_.y);
  return {x0, y0, x1 - x0, y1 - y0};
}

void DocumentView::cancel_shape_drag() {
  if (drag_ == Drag::kShape) drag_ = Drag::kNone;
  shape_page_ = -1;
}

MarkupItems DocumentView::selection_markup_items() const {
  MarkupItems items;
  if (selection_.empty()) return items;
  for (int page : selection_.pages()) {
    const view::PageTextIndex* index = text_index(page);
    if (index == nullptr) continue;
    const view::TextRange range = selection_.range_on(page, index->char_count());
    if (range.empty()) continue;
    std::vector<onpdf::Rect> rects = index->selection_rects(range);
    if (!rects.empty()) items.emplace_back(page, std::move(rects));
  }
  return items;
}

std::optional<engine::AnnotationInfo> DocumentView::selected_annotation() const {
  if (!selected_annot_) return std::nullopt;
  return selected_annot_->info;
}

void DocumentView::clear_annotation_selection() {
  const bool pending = reselect_page_ >= 0;  // 조용히 재선택을 기다리던 중이면 해제 신호는 지금 낸다
  reselect_page_ = -1;
  reselect_id_.clear();
  if (!selected_annot_ && !pending) return;
  const int page = selected_annot_ ? selected_annot_->page : -1;
  selected_annot_.reset();
  emit annotation_selected(false, engine::AnnotationInfo{});
  if (page >= 0) update_page(page);
}

void DocumentView::paint_annotations(QPainter& painter, int page, const QRectF& page_logical) {
  const double k = layout_->scale() / dpr();  // 포인트 → 논리 px
  if (selected_annot_ && selected_annot_->page == page) {
    const onpdf::Rect& r = selected_annot_->info.rect;
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(kAnnotationOutline, 2));
    painter.drawRect(QRectF(page_logical.x() + r.x * k, page_logical.y() + r.y * k, r.w * k, r.h * k).adjusted(-2, -2, 2, 2));
  }
  if (selected_text_ && selected_text_->page == page) {
    const onpdf::Rect& r = selected_text_->bbox;
    const QRectF box(page_logical.x() + r.x * k, page_logical.y() + r.y * k, r.w * k, r.h * k);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(kAnnotationOutline, 2));
    painter.drawRect(box.adjusted(-2, -2, 2, 2));
    if (drag_ == Drag::kMoveText) {
      painter.setPen(QPen(kAnnotationOutline, 1, Qt::DashLine));
      painter.drawRect(box.translated(move_delta_.x * k, move_delta_.y * k));
    }
  }
  if (drag_ == Drag::kInk && ink_page_ == page && ink_points_.size() >= 2) {
    QPen pen(kInkPreview, std::max(1.0, 2.0 * k));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    QPolygonF poly;
    for (const onpdf::Point& p : ink_points_) poly << QPointF(page_logical.x() + p.x * k, page_logical.y() + p.y * k);
    painter.drawPolyline(poly);
  }
  if (drag_ == Drag::kShape && shape_page_ == page) {
    const onpdf::Rect r = shape_rect();
    QPen pen(kInkPreview, std::max(1.0, 2.0 * k), Qt::DashLine);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    const QRectF box(page_logical.x() + r.x * k, page_logical.y() + r.y * k, r.w * k, r.h * k);
    if (tool_ == AnnotationTool::kCircle) painter.drawEllipse(box);
    else painter.drawRect(box);
  }
}

void DocumentView::on_text_index(int page, int generation, std::shared_ptr<const view::PageTextIndex> index,
                                 bool cancelled) {
  if (generation != doc_generation_) return;  // 이전 문서
  text_requested_.erase(page);
  if (cancelled) return;  // 취소된 요청은 다시 보이면 재요청된다
  if (!index) index = std::make_shared<const view::PageTextIndex>();  // 추출 실패: 빈 인덱스로 재요청 방지
  text_cache_[page] = std::move(index);
  resolve_pending_drag(page, *text_cache_[page]);
  emit text_index_ready(page);
  if (layout_ && page < layout_->page_count()) {
    const double d = dpr();
    const onpdf::Rect r = layout_->page_rect(page);
    viewport()->update(QRectF(horizontal_offset_logical() + r.x / d - horizontalScrollBar()->value(),
                              r.y / d - verticalScrollBar()->value(), r.w / d, r.h / d)
                           .toAlignedRect());
  }
}

const view::PageTextIndex* DocumentView::text_index(int page) const {
  auto it = text_cache_.find(page);
  return it == text_cache_.end() ? nullptr : it->second.get();
}

// ---------------------------------------------------------------- 좌표

onpdf::Point DocumentView::page_point_on(int page, QPointF viewport_pos) const {
  const double d = dpr();
  const onpdf::Point doc{(viewport_pos.x() - horizontal_offset_logical() + horizontalScrollBar()->value()) * d,
                         (viewport_pos.y() + verticalScrollBar()->value()) * d};
  return layout_->page_px_to_pt(page, layout_->doc_to_page_px(page, doc));
}

std::optional<std::pair<int, onpdf::Point>> DocumentView::page_point_at(QPointF viewport_pos) const {
  if (!layout_ || layout_->page_count() == 0) return std::nullopt;
  const double d = dpr();
  const onpdf::Point doc{(viewport_pos.x() - horizontal_offset_logical() + horizontalScrollBar()->value()) * d,
                         (viewport_pos.y() + verticalScrollBar()->value()) * d};
  const int page = layout_->page_at(doc.y);
  if (page < 0) return std::nullopt;
  const onpdf::Point in_page = layout_->doc_to_page_px(page, doc);
  return std::make_pair(page, layout_->page_px_to_pt(page, in_page));
}

std::optional<std::pair<int, onpdf::Point>> DocumentView::reading_position() const {
  if (!layout_ || layout_->page_count() == 0) return std::nullopt;
  // 이동 함수들은 대상을 여백(margin) 만큼 아래에 둔다 → 그보다 1 논리 px 아래를 본다
  const double probe_y = layout_->options().margin / dpr() + 1.0;
  auto hit = page_point_at(QPointF(viewport()->width() / 2.0, probe_y));
  if (!hit) return std::nullopt;
  auto& [page, pt] = *hit;
  if (pt.y > layout_->page_size_pt(page).height && page + 1 < layout_->page_count()) {
    page += 1;  // 간격은 위쪽 쪽에 귀속되므로 다음 쪽 맨 위로 본다
    pt = {0.0, 0.0};
  }
  return hit;
}

QPointF DocumentView::page_point_to_viewport(int page, onpdf::Point pt) const {
  if (!layout_) return {};
  const double d = dpr();
  const double s = layout_->scale();
  const onpdf::Point doc = layout_->page_px_to_doc(page, {pt.x * s, pt.y * s});
  return {horizontal_offset_logical() + doc.x / d - horizontalScrollBar()->value(),
          doc.y / d - verticalScrollBar()->value()};
}

view::TextPos DocumentView::position_at(QPointF viewport_pos) const {
  auto hit = page_point_at(viewport_pos);
  if (!hit) return {};
  const auto [page, pt] = *hit;
  const view::PageTextIndex* index = text_index(page);
  if (index == nullptr) return {page, 0};
  return {page, index->nearest_position(pt)};
}

void DocumentView::scroll_to_page_point(int page, onpdf::Point pt) {
  if (!layout_ || layout_->page_count() == 0) return;
  page = std::clamp(page, 0, layout_->page_count() - 1);
  const double d = dpr();
  const double s = layout_->scale();
  const onpdf::Point doc = layout_->page_px_to_doc(page, {pt.x * s, pt.y * s});
  scroll_to_anchored(page, doc.y / d - layout_->options().margin / d);
}

// ---------------------------------------------------------------- 선택

void DocumentView::set_selection(view::TextPos anchor, view::TextPos focus) {
  const bool before = has_selection();
  const QRegion old_region = selection_region();
  selection_.set(anchor, focus);
  // 바뀐 선택 사각형만 다시 그린다 (긴 문서에서 드래그 중 전체 다시 그리기 방지)
  const QRegion dirty = old_region.united(selection_region());
  if (!dirty.isEmpty()) viewport()->update(dirty);
  if (before != has_selection()) emit selection_changed(has_selection());
}

void DocumentView::clear_selection() {
  pending_anchor_.reset();
  pending_focus_.reset();
  if (selection_.empty()) return;
  const QRegion old_region = selection_region();
  selection_.clear();
  if (!old_region.isEmpty()) viewport()->update(old_region);
  emit selection_changed(false);
}

QRegion DocumentView::selection_region() const {
  QRegion region;
  if (!layout_ || selection_.empty()) return region;
  const double d = dpr();
  const double k = layout_->scale() / d;  // 포인트 → 논리 px
  const double ox = horizontal_offset_logical() - horizontalScrollBar()->value();
  const double oy = -verticalScrollBar()->value();
  for (int page : layout_->pages_in(viewport_doc())) {
    const view::PageTextIndex* index = text_index(page);
    if (index == nullptr) continue;
    const view::TextRange range = selection_.range_on(page, index->char_count());
    if (range.empty()) continue;
    const onpdf::Rect pr = layout_->page_rect(page);
    const double px = ox + pr.x / d;
    const double py = oy + pr.y / d;
    for (const onpdf::Rect& r : index->selection_rects(range)) {
      region += QRectF(px + r.x * k, py + r.y * k, r.w * k, r.h * k).toAlignedRect().adjusted(-1, -1, 1, 1);
    }
  }
  return region;
}

view::TextPos DocumentView::drag_position(QPointF viewport_pos,
                                          std::optional<std::pair<int, onpdf::Point>>& pending) {
  auto hit = page_point_at(viewport_pos);
  if (!hit) {
    pending.reset();
    return {};
  }
  const auto [page, pt] = *hit;
  if (const view::PageTextIndex* index = text_index(page)) {
    pending.reset();
    return {page, index->nearest_position(pt)};
  }
  pending = *hit;  // 인덱스가 오면 resolve_pending_drag 가 실제 위치로 바꾼다
  return {page, 0};
}

void DocumentView::resolve_pending_drag(int page, const view::PageTextIndex& index) {
  view::TextPos anchor = selection_.anchor();
  view::TextPos focus = selection_.focus();
  bool changed = false;
  if (pending_anchor_ && pending_anchor_->first == page) {
    anchor = {page, index.nearest_position(pending_anchor_->second)};
    pending_anchor_.reset();
    changed = true;
  }
  if (pending_focus_ && pending_focus_->first == page) {
    focus = {page, index.nearest_position(pending_focus_->second)};
    pending_focus_.reset();
    changed = true;
  }
  if (changed && anchor.page >= 0) set_selection(anchor, focus);
}

void DocumentView::select_all_on_current_page() {
  pending_anchor_.reset();
  pending_focus_.reset();
  const int page = current_page();
  const view::PageTextIndex* index = text_index(page);
  if (page < 0 || index == nullptr || index->empty()) return;
  set_selection({page, 0}, {page, index->char_count()});
}

QString DocumentView::selected_text(bool* truncated) const {
  if (truncated) *truncated = false;
  if (selection_.empty()) return {};
  // 미로드 쪽은 워커에 동기 요청 (복사 시점에만, 상한 있음)
  std::unordered_map<int, std::shared_ptr<const view::PageTextIndex>> loaded;
  int budget = kMaxCopyPages;
  for (int page : selection_.pages()) {
    if (text_index(page) != nullptr) continue;
    if (budget-- <= 0) {
      if (truncated) *truncated = true;
      break;
    }
    auto result = handle_.load_text(page, engine::Priority::kHigh).get();
    if (result) loaded[page] = std::make_shared<const view::PageTextIndex>(std::move(*result));
  }
  const std::string text = selection_.text([&](int page) -> const view::PageTextIndex* {
    if (const auto* cached = text_index(page)) return cached;
    auto it = loaded.find(page);
    return it == loaded.end() ? nullptr : it->second.get();
  });
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

void DocumentView::paint_selection(QPainter& painter, int page, const QRectF& page_logical) {
  if (selection_.empty()) return;
  const view::PageTextIndex* index = text_index(page);
  if (index == nullptr) return;
  const view::TextRange range = selection_.range_on(page, index->char_count());
  if (range.empty()) return;
  const double k = layout_->scale() / dpr();  // 포인트 → 논리 px
  painter.setPen(Qt::NoPen);
  painter.setBrush(kSelectionColor);
  for (const onpdf::Rect& r : index->selection_rects(range)) {
    painter.drawRect(QRectF(page_logical.x() + r.x * k, page_logical.y() + r.y * k, r.w * k, r.h * k));
  }
}

// ---------------------------------------------------------------- 검색

void DocumentView::set_search(const view::SearchSession* search) {
  search_ = search;
  viewport()->update();
}

void DocumentView::paint_search(QPainter& painter, int page, const QRectF& page_logical) {
  if (search_ == nullptr || !search_->active()) return;
  const std::vector<view::TextRange>* hits = search_->hits_on(page);
  const std::vector<std::vector<onpdf::Rect>>* rects = search_->rects_on(page);
  if (hits == nullptr || rects == nullptr) return;
  const auto current = search_->current();
  const double k = layout_->scale() / dpr();
  painter.setPen(Qt::NoPen);
  for (std::size_t i = 0; i < hits->size() && i < rects->size(); ++i) {
    const bool is_current = current && current->page == page && current->range == (*hits)[i];
    painter.setBrush(is_current ? kSearchCurrentColor : kSearchHitColor);
    for (const onpdf::Rect& r : (*rects)[i]) {
      painter.drawRect(QRectF(page_logical.x() + r.x * k, page_logical.y() + r.y * k, r.w * k, r.h * k));
    }
  }
}

void DocumentView::reveal(int page, const std::vector<onpdf::Rect>& rects) {
  if (!layout_ || page < 0 || page >= layout_->page_count()) return;
  if (rects.empty()) {
    go_to_page(page);
    return;
  }
  const onpdf::Rect& r = rects.front();
  const QPointF top_left = page_point_to_viewport(page, {r.x, r.y});
  const QPointF bottom_right = page_point_to_viewport(page, {r.right(), r.bottom()});
  const QRectF target(top_left, bottom_right);
  const QRectF vp(viewport()->rect());
  if (!vp.contains(target)) {
    if (target.top() < vp.top() || target.bottom() > vp.bottom()) {
      verticalScrollBar()->setValue(verticalScrollBar()->value() +
                                    static_cast<int>(std::lround(target.top() - vp.height() / 3.0)));
    }
    if (target.left() < vp.left() || target.right() > vp.right()) {
      horizontalScrollBar()->setValue(horizontalScrollBar()->value() +
                                      static_cast<int>(std::lround(target.left() - vp.width() / 4.0)));
    }
  }
  viewport()->update();
}

void DocumentView::update_hover(QPointF viewport_pos) {
  QString target;
  Qt::CursorShape cursor = Qt::ArrowCursor;
  if (tool_ == AnnotationTool::kNote || tool_ == AnnotationTool::kPen || tool_ == AnnotationTool::kSquare ||
      tool_ == AnnotationTool::kCircle) {
    cursor = Qt::CrossCursor;
  }
  if (tool_ == AnnotationTool::kText) cursor = Qt::IBeamCursor;
  QString tooltip;
  if (auto hit = page_point_at(viewport_pos)) {
    const auto [page, pt] = *hit;
    if (tool_ == AnnotationTool::kSelect) {
      if (const engine::AnnotationInfo* a = annotation_at(page, pt)) {
        cursor = Qt::PointingHandCursor;
        if (a->kind == engine::AnnotationKind::kText && !a->contents.empty()) tooltip = QString::fromStdString(a->contents);
      } else if (selected_text_ && selected_text_->page == page && selected_text_->bbox.contains(pt)) {
        cursor = Qt::SizeAllCursor;  // 선택한 상자 안: 끌어 옮길 수 있다
      } else if (text_box_at(page, pt)) {
        cursor = Qt::PointingHandCursor;
      }
    }
    if (const view::PageTextIndex* index = text_index(page); index != nullptr && cursor == Qt::ArrowCursor) {
      if (const view::Link* link = index->link_at(pt)) {
        cursor = Qt::PointingHandCursor;
        target = link->kind == view::LinkKind::kUri ? QString::fromStdString(link->uri)
                                                     : tr("%1 쪽").arg(link->page + 1);
      } else if (index->hit_test(pt)) {
        cursor = Qt::IBeamCursor;
      }
    }
  }
  viewport()->setCursor(cursor);
  if (!tooltip.isEmpty()) {
    QToolTip::showText(viewport()->mapToGlobal(viewport_pos.toPoint()), tooltip, viewport());
  } else if (QToolTip::isVisible()) {
    QToolTip::hideText();
  }
  if (target != hovered_target_) {
    hovered_target_ = target;
    emit link_hovered(target);
  }
}

void DocumentView::autoscroll_step() {
  if (drag_ != Drag::kText) {
    autoscroll_timer_.stop();
    return;
  }
  const QRect vp = viewport()->rect();
  int dy = 0;
  if (last_drag_pos_.y() < vp.top()) dy = static_cast<int>(last_drag_pos_.y() - vp.top());
  if (last_drag_pos_.y() > vp.bottom()) dy = static_cast<int>(last_drag_pos_.y() - vp.bottom());
  if (dy == 0) {
    autoscroll_timer_.stop();
    return;
  }
  verticalScrollBar()->setValue(verticalScrollBar()->value() + std::clamp(dy, -40, 40));
  set_selection(selection_.anchor(), drag_position(last_drag_pos_, pending_focus_));
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
  QAbstractScrollArea::scrollContentsBy(dx, dy);  // viewport()->update() 포함
  // 사용자 스크롤·배율 변경 등으로 고정한 값에서 벗어나면 이동 고정 쪽을 버리고 중앙 기준으로 돌아간다
  if (nav_anchor_ && nav_anchor_->scroll != verticalScrollBar()->value()) nav_anchor_.reset();
  schedule_update();
  emit_page_if_changed();
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
  if (event->key() == Qt::Key_Escape) {
    clear_selection();
    clear_annotation_selection();
    clear_text_box_selection();
    if (drag_ == Drag::kInk) {
      drag_ = Drag::kNone;
      ink_points_.clear();
      viewport()->update();
    }
    if (drag_ == Drag::kShape) {
      cancel_shape_drag();
      viewport()->update();
    }
    event->accept();
    return;
  }
  if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && selected_annot_) {
    emit annotation_delete_requested(selected_annot_->page, QString::fromStdString(selected_annot_->info.id));
    event->accept();
    return;
  }
  if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && selected_text_) {
    emit text_delete_requested(selected_text_->page, QString::fromStdString(selected_text_->box));
    event->accept();
    return;
  }
  switch (event->key()) {
    case Qt::Key_PageDown: v->setValue(v->value() + v->pageStep()); break;
    case Qt::Key_PageUp: v->setValue(v->value() - v->pageStep()); break;
    case Qt::Key_Home: v->setValue(v->minimum()); break;
    case Qt::Key_End: v->setValue(v->maximum()); break;
    default: QAbstractScrollArea::keyPressEvent(event); return;
  }
  event->accept();
}

void DocumentView::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton || !layout_) {
    QAbstractScrollArea::mousePressEvent(event);
    return;
  }
  setFocus();
  // 새 클릭은 이전 드래그의 보류 지점을 버린다 (늦게 온 인덱스가 새 선택을 덮어쓰지 않게)
  pending_anchor_.reset();
  pending_focus_.reset();
  press_pos_ = event->position();
  // 연속 클릭 수 (더블/트리플)
  const bool near_last = (event->position() - last_click_pos_).manhattanLength() < 6;
  click_count_ = (click_timer_.isValid() && click_timer_.elapsed() < kMultiClickMs && near_last) ? click_count_ + 1 : 1;
  click_timer_.restart();
  last_click_pos_ = event->position();

  pressed_link_.reset();
  // ---- 주석 도구 ----
  if (tool_ == AnnotationTool::kNote) {
    if (auto hit = page_point_at(event->position())) {
      const auto [page, pt] = *hit;
      emit note_requested(page, clamp_to_page(page, pt));
    }
    return;
  }
  if (tool_ == AnnotationTool::kText) {
    if (auto hit = page_point_at(event->position())) {
      const auto [page, pt] = *hit;
      emit text_requested(page, clamp_to_page(page, pt));
    }
    return;
  }
  if (tool_ == AnnotationTool::kPen) {
    if (auto hit = page_point_at(event->position())) {
      const auto [page, pt] = *hit;
      ink_page_ = page;
      ink_points_ = {clamp_to_page(page, pt)};
      drag_ = Drag::kInk;
    }
    return;
  }
  if (tool_ == AnnotationTool::kSquare || tool_ == AnnotationTool::kCircle) {
    if (auto hit = page_point_at(event->position())) {
      const auto [page, pt] = *hit;
      shape_page_ = page;
      shape_anchor_ = shape_corner_ = clamp_to_page(page, pt);
      drag_ = Drag::kShape;
    }
    return;
  }
  if (tool_ == AnnotationTool::kSelect) {
    if (auto hit = page_point_at(event->position())) {
      const auto [page, pt] = *hit;
      if (const engine::AnnotationInfo* a = annotation_at(page, pt)) {
        const bool same = selected_annot_ && selected_annot_->page == page && selected_annot_->info.id == a->id;
        const int previous_page = selected_annot_ ? selected_annot_->page : -1;
        const engine::AnnotationInfo info = *a;  // 캐시가 바뀌어도 안전하게 복사
        selected_annot_ = SelectedAnnotation{page, info};
        clear_selection();
        if (previous_page >= 0 && previous_page != page) update_page(previous_page);
        update_page(page);
        emit annotation_selected(true, info);
        if (same && click_count_ == 2) emit annotation_edit_requested(page, info);
        drag_ = Drag::kNone;
        return;
      }
    }
    if (selected_annot_) clear_annotation_selection();
    if (auto hit = page_point_at(event->position())) {
      const auto [page, pt] = *hit;
      // 선택된 상자 안을 누르면 이동 드래그, 다른 상자를 누르면 그 상자 선택, 두 번 클릭은 편집
      if (selected_text_ && selected_text_->page == page && selected_text_->bbox.contains(pt)) {
        if (click_count_ == 2) {
          emit text_edit_requested(page, *selected_text_);
          drag_ = Drag::kNone;
          return;
        }
        move_start_pt_ = pt;
        move_delta_ = {};
        drag_ = Drag::kMoveText;
        return;
      }
      if (auto box = text_box_at(page, pt)) {
        const int previous_page = selected_text_ ? selected_text_->page : -1;
        selected_text_ = box;
        clear_selection();
        if (previous_page >= 0 && previous_page != page) update_page(previous_page);
        update_page(page);
        emit text_box_selected(true, *selected_text_);
        drag_ = Drag::kNone;
        return;
      }
    }
    if (selected_text_) clear_text_box_selection();
  }
  if (auto hit = page_point_at(event->position())) {
    const auto [page, pt] = *hit;
    if (const view::PageTextIndex* index = text_index(page)) {
      // 링크는 단일 클릭에서만 활성화 (더블클릭이 두 번 열지 않도록). 마크업 도구에서는 링크를 열지 않는다
      if (const view::Link* link = index->link_at(pt);
          link != nullptr && click_count_ == 1 && tool_ == AnnotationTool::kSelect) {
        pressed_link_ = *link;
        drag_ = Drag::kMaybeLink;
        return;
      }
      if (click_count_ >= 3) {
        // 글자 위면 그 글자의 줄, 아니면 가장 가까운 위치의 줄 (줄 끝은 그 줄에 귀속)
        int ch = index->hit_test(pt).value_or(-1);
        if (ch < 0) {
          const int pos = index->nearest_position(pt);
          ch = std::clamp(pos > 0 ? pos - 1 : 0, 0, std::max(0, index->char_count() - 1));
        }
        const view::TextRange line = index->line_at(ch);
        if (!line.empty()) set_selection({page, line.start}, {page, line.end});
        drag_ = Drag::kNone;
        return;
      }
      if (click_count_ == 2) {
        if (auto ch = index->hit_test(pt)) {
          const view::TextRange word = index->word_at(*ch);
          set_selection({page, word.start}, {page, word.end});
          drag_ = Drag::kNone;
          return;
        }
      }
    }
  }
  pending_focus_.reset();
  const view::TextPos anchor = drag_position(event->position(), pending_anchor_);
  if (anchor.page < 0) {
    clear_selection();
    return;
  }
  drag_ = Drag::kText;
  set_selection(anchor, anchor);  // 빈 선택으로 시작
}

void DocumentView::mouseMoveEvent(QMouseEvent* event) {
  if (drag_ == Drag::kInk) {
    if (ink_page_ >= 0) {
      // 다른 쪽 위로 나가도 시작한 쪽의 좌표로 투영한 뒤 쪽 안으로 클램프한다 (옆 쪽 좌표를 자르면 가장자리로 튄다)
      const onpdf::Point previous = ink_points_.back();
      const onpdf::Point next = clamp_to_page(ink_page_, page_point_on(ink_page_, event->position()));
      ink_points_.push_back(next);
      // 방금 추가된 선분 영역만 다시 그린다 (쪽 전체 갱신은 고배율에서 느리다)
      const QPointF a = page_point_to_viewport(ink_page_, previous);
      const QPointF b = page_point_to_viewport(ink_page_, next);
      const double margin = std::max(6.0, 2.0 * layout_->scale() / dpr()) + 2.0;
      viewport()->update(QRectF(a, b).normalized().adjusted(-margin, -margin, margin, margin).toAlignedRect());
    }
    return;
  }
  if (drag_ == Drag::kMoveText) {
    if (selected_text_) {
      const int page = selected_text_->page;
      onpdf::Point now = page_point_on(page, event->position());
      onpdf::Point delta{now.x - move_start_pt_.x, now.y - move_start_pt_.y};
      // 상자가 쪽 밖으로 나가지 않게 Δ 를 클램프
      const engine::PageSize size = layout_->page_size_pt(page);
      const onpdf::Rect& b = selected_text_->bbox;
      delta.x = std::clamp(delta.x, -b.x, std::max(-b.x, size.width - b.right()));
      delta.y = std::clamp(delta.y, -b.y, std::max(-b.y, size.height - b.bottom()));
      move_delta_ = delta;
      update_page(page);
    }
    return;
  }
  if (drag_ == Drag::kShape) {
    if (shape_page_ >= 0) {
      // 이전·현재 도형 영역만 다시 그린다 (펜과 같은 이유). 시작한 쪽 좌표로 투영해 쪽 안으로 클램프
      const onpdf::Rect before = shape_rect();
      shape_corner_ = clamp_to_page(shape_page_, page_point_on(shape_page_, event->position()));
      const onpdf::Rect after = shape_rect();
      const double margin = std::max(6.0, 2.0 * layout_->scale() / dpr()) + 2.0;
      for (const onpdf::Rect& r : {before, after}) {
        const QPointF a = page_point_to_viewport(shape_page_, {r.x, r.y});
        const QPointF b = page_point_to_viewport(shape_page_, {r.right(), r.bottom()});
        viewport()->update(QRectF(a, b).normalized().adjusted(-margin, -margin, margin, margin).toAlignedRect());
      }
    }
    return;
  }
  if (drag_ == Drag::kText) {
    last_drag_pos_ = event->position();
    set_selection(selection_.anchor(), drag_position(event->position(), pending_focus_));
    const QRect vp = viewport()->rect();
    if (!vp.contains(event->position().toPoint())) {
      if (!autoscroll_timer_.isActive()) autoscroll_timer_.start();
    } else {
      autoscroll_timer_.stop();
    }
    return;
  }
  if (drag_ == Drag::kMaybeLink) {
    if ((event->position() - press_pos_).manhattanLength() >= kLinkClickSlop) {
      // 링크 위에서 끌면 텍스트 선택으로 전환
      drag_ = Drag::kText;
      pressed_link_.reset();
      const view::TextPos anchor = drag_position(press_pos_, pending_anchor_);
      set_selection(anchor, drag_position(event->position(), pending_focus_));
    }
    return;
  }
  update_hover(event->position());
}

void DocumentView::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) {
    QAbstractScrollArea::mouseReleaseEvent(event);
    return;
  }
  autoscroll_timer_.stop();
  if (drag_ == Drag::kInk) {
    drag_ = Drag::kNone;
    const int page = ink_page_;
    std::vector<onpdf::Point> points = std::move(ink_points_);
    ink_points_.clear();
    ink_page_ = -1;
    if (page >= 0) update_page(page);
    if (page >= 0 && points.size() >= 2) {
      double x0 = points[0].x, y0 = points[0].y, x1 = x0, y1 = y0;
      for (const auto& p : points) {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
      }
      emit ink_requested(page, points, onpdf::Rect{x0 - 2, y0 - 2, x1 - x0 + 4, y1 - y0 + 4});
    }
    return;
  }
  if (drag_ == Drag::kMoveText) {
    drag_ = Drag::kNone;
    const onpdf::Point delta = move_delta_;
    move_delta_ = {};
    if (selected_text_) {
      update_page(selected_text_->page);
      if (std::abs(delta.x) >= kMoveThresholdPt || std::abs(delta.y) >= kMoveThresholdPt) {
        emit text_move_requested(selected_text_->page, selected_text_->ids, delta);
      }
    }
    return;
  }
  if (drag_ == Drag::kShape) {
    drag_ = Drag::kNone;
    const int page = shape_page_;
    const onpdf::Rect rect = shape_rect();
    shape_page_ = -1;
    if (page >= 0) update_page(page);
    // 실수로 누른 클릭(4pt 미만)은 도형을 만들지 않는다
    if (page >= 0 && rect.w >= 4.0 && rect.h >= 4.0) {
      emit shape_requested(page, tool_ == AnnotationTool::kSquare ? engine::AnnotationKind::kSquare : engine::AnnotationKind::kCircle,
                           rect);
    }
    return;
  }
  const bool was_text_drag = drag_ == Drag::kText;
  if (drag_ == Drag::kMaybeLink && pressed_link_) {
    const view::Link link = *pressed_link_;
    pressed_link_.reset();
    drag_ = Drag::kNone;
    emit link_activated(link);
    return;
  }
  drag_ = Drag::kNone;
  update_hover(event->position());
  // 마크업 도구: 드래그를 놓으면 선택 범위가 바로 주석이 된다 (인덱스 없는 쪽은 빠진다)
  if (was_text_drag && tool_ != AnnotationTool::kSelect && !selection_.empty()) {
    const MarkupItems items = selection_markup_items();
    if (!items.empty()) {
      const engine::AnnotationKind kind = tool_ == AnnotationTool::kHighlight   ? engine::AnnotationKind::kHighlight
                                          : tool_ == AnnotationTool::kUnderline ? engine::AnnotationKind::kUnderline
                                                                                : engine::AnnotationKind::kStrikeOut;
      emit markup_requested(kind, items);
    }
  }
}

void DocumentView::mouseDoubleClickEvent(QMouseEvent* event) {
  // Qt 가 두 번째 press 대신 보내는 이벤트. 우리 클릭 카운터로 처리하도록 press 로 넘긴다
  mousePressEvent(event);
}

void DocumentView::leaveEvent(QEvent* event) {
  QAbstractScrollArea::leaveEvent(event);
  if (!hovered_target_.isEmpty()) {
    hovered_target_.clear();
    emit link_hovered({});
  }
}

bool DocumentView::viewportEvent(QEvent* event) {
  // 뷰포트를 떠나면(스크롤바 위로 가도) 호버 상태를 지운다 — Leave 는 viewport 에 오고 서브클래스 leaveEvent 로 오지 않는다
  if (event->type() == QEvent::Leave && !hovered_target_.isEmpty()) {
    hovered_target_.clear();
    emit link_hovered({});
  }
  // 네이티브 제스처(트랙패드 핀치)는 viewport 위젯에 오며, QAbstractScrollArea 는 이를
  // 서브클래스 event() 로 넘기지 않으므로 여기서 받는다
  if (event->type() == QEvent::NativeGesture) {
    auto* gesture = static_cast<QNativeGestureEvent*>(event);
    if (gesture->gestureType() == Qt::ZoomNativeGesture) {
      set_zoom(zoom_ * (1.0 + gesture->value()), gesture->position());
      return true;
    }
  }
  return QAbstractScrollArea::viewportEvent(event);
}

bool DocumentView::event(QEvent* event) {
  if (event->type() == QEvent::DevicePixelRatioChange) {
    // 다른 DPR 모니터로 이동: 타일 배율을 새 dpr 로 다시 계산
    relayout();
  }
  return QAbstractScrollArea::event(event);
}

}  // namespace onpdf::app
