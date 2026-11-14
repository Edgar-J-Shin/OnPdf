#include "document_view.h"

#include "document_tile_source.h"
#include "onpdf/core/view/tile.h"
#include "qt_image.h"

#include <QApplication>
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
  first_view_pending_ = true;
  relayout();
  emit_page_if_changed();
}

void DocumentView::clear_document(const QString& placeholder) {
  scheduler_.reset();
  source_.reset();
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

  // 4. 선택 하이라이트
  paint_selection(painter, page, page_logical);
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
}

void DocumentView::on_text_index(int page, int generation, std::shared_ptr<const view::PageTextIndex> index,
                                 bool cancelled) {
  if (generation != doc_generation_) return;  // 이전 문서
  text_requested_.erase(page);
  if (cancelled) return;  // 취소된 요청은 다시 보이면 재요청된다
  if (!index) index = std::make_shared<const view::PageTextIndex>();  // 추출 실패: 빈 인덱스로 재요청 방지
  text_cache_[page] = std::move(index);
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
  verticalScrollBar()->setValue(static_cast<int>(std::lround(doc.y / d - layout_->options().margin / d)));
}

// ---------------------------------------------------------------- 선택

void DocumentView::set_selection(view::TextPos anchor, view::TextPos focus) {
  const bool before = has_selection();
  selection_.set(anchor, focus);
  viewport()->update();
  if (before != has_selection()) emit selection_changed(has_selection());
}

void DocumentView::clear_selection() {
  if (selection_.empty()) return;
  selection_.clear();
  viewport()->update();
  emit selection_changed(false);
}

void DocumentView::select_all_on_current_page() {
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

void DocumentView::update_hover(QPointF viewport_pos) {
  QString target;
  Qt::CursorShape cursor = Qt::ArrowCursor;
  if (auto hit = page_point_at(viewport_pos)) {
    const auto [page, pt] = *hit;
    if (const view::PageTextIndex* index = text_index(page)) {
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
  set_selection(selection_.anchor(), position_at(last_drag_pos_));
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
  press_pos_ = event->position();
  // 연속 클릭 수 (더블/트리플)
  const bool near_last = (event->position() - last_click_pos_).manhattanLength() < 6;
  click_count_ = (click_timer_.isValid() && click_timer_.elapsed() < kMultiClickMs && near_last) ? click_count_ + 1 : 1;
  click_timer_.restart();
  last_click_pos_ = event->position();

  pressed_link_.reset();
  if (auto hit = page_point_at(event->position())) {
    const auto [page, pt] = *hit;
    if (const view::PageTextIndex* index = text_index(page)) {
      // 링크는 단일 클릭에서만 활성화 (더블클릭이 두 번 열지 않도록)
      if (const view::Link* link = index->link_at(pt); link != nullptr && click_count_ == 1) {
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
  const view::TextPos anchor = position_at(event->position());
  if (anchor.page < 0) {
    clear_selection();
    return;
  }
  drag_ = Drag::kText;
  set_selection(anchor, anchor);  // 빈 선택으로 시작
}

void DocumentView::mouseMoveEvent(QMouseEvent* event) {
  if (drag_ == Drag::kText) {
    last_drag_pos_ = event->position();
    set_selection(selection_.anchor(), position_at(event->position()));
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
      set_selection(position_at(press_pos_), position_at(event->position()));
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
  if (drag_ == Drag::kMaybeLink && pressed_link_) {
    const view::Link link = *pressed_link_;
    pressed_link_.reset();
    drag_ = Drag::kNone;
    emit link_activated(link);
    return;
  }
  drag_ = Drag::kNone;
  update_hover(event->position());
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
