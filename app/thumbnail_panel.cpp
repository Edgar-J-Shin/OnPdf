#include "thumbnail_panel.h"

#include "qt_image.h"
#include "ui_dispatcher.h"

#include <QAbstractListModel>
#include <QKeyEvent>
#include <QListView>
#include <QPainter>
#include <QPointer>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

namespace onpdf::app {
namespace {

constexpr int kLabelHeight = 20;
constexpr int kPadding = 6;      // 썸네일 위아래 여백
constexpr int kSideMargin = 16;  // 썸네일 좌우 여백 (목록 배경이 보이게)
constexpr int kMinThumb = 48;
constexpr int kMaxThumb = 360;
constexpr double kMaxAspect = 4.0;  // 아주 긴 쪽은 높이를 폭의 4배로 제한

}  // namespace

// 행 수만 알려 주는 모델. 그림은 델리게이트가 패널에서 직접 가져온다.
class ThumbnailModel : public QAbstractListModel {
 public:
  using QAbstractListModel::QAbstractListModel;
  void reset_rows(int rows) {
    beginResetModel();
    rows_ = rows;
    endResetModel();
  }
  void relayout() {
    emit layoutAboutToBeChanged();
    emit layoutChanged();
  }
  void page_changed(int page) {
    const QModelIndex i = index(page);
    emit dataChanged(i, i);
  }
  [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override {
    return parent.isValid() ? 0 : rows_;
  }
  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override {
    if (!index.isValid()) return {};
    if (role == Qt::DisplayRole || role == Qt::ToolTipRole) return QString::number(index.row() + 1);
    return {};
  }

 private:
  int rows_ = 0;
};

namespace {

// 키보드로 현재 행을 옮길 때만 쪽 이동으로 본다. (포커스·모델 리셋 때 Qt 가 스스로 현재 행을 정하는 경우는 무시)
class ThumbnailList : public QListView {
 public:
  using QListView::QListView;
  [[nodiscard]] bool keyboard_navigating() const noexcept { return keyboard_; }

 protected:
  void keyPressEvent(QKeyEvent* event) override {
    const QScopedValueRollback guard(keyboard_, true);
    QListView::keyPressEvent(event);
  }

 private:
  bool keyboard_ = false;
};

class ThumbnailDelegate : public QStyledItemDelegate {
 public:
  explicit ThumbnailDelegate(ThumbnailPanel* panel) : QStyledItemDelegate(panel), panel_(panel) {}

  QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const override {
    const QSize thumb = panel_->thumb_size(index.row());
    return {panel_->thumb_width() + 2 * kSideMargin, thumb.height() + kLabelHeight + 2 * kPadding};
  }

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
    const int page = index.row();
    const QSize size = panel_->thumb_size(page);
    const QRect area = option.rect;
    const QRect box(area.left() + (area.width() - size.width()) / 2, area.top() + kPadding, size.width(),
                    size.height());
    painter->save();
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    const QImage image = panel_->thumbnail(page);
    if (image.isNull()) {
      painter->fillRect(box, Qt::white);
    } else {
      painter->drawImage(QRectF(box), image);  // 이전 폭의 썸네일은 늘려 그린다
    }
    const bool current = (option.state & QStyle::State_Selected) != 0;
    if (current) {
      painter->setPen(QPen(option.palette.color(QPalette::Highlight), 3));
      painter->drawRect(box.adjusted(-2, -2, 1, 1));
    } else {
      painter->setPen(QPen(option.palette.color(QPalette::Mid), 1));
      painter->drawRect(box.adjusted(0, 0, -1, -1));
    }
    const QRect label(area.left(), box.bottom() + 2, area.width(), kLabelHeight);
    QFont font = option.font;
    font.setBold(current);
    painter->setFont(font);
    painter->setPen(option.palette.color(QPalette::Text));
    painter->drawText(label, Qt::AlignHCenter | Qt::AlignVCenter, QString::number(page + 1));
    painter->restore();
  }

 private:
  ThumbnailPanel* panel_;
};

}  // namespace

ThumbnailPanel::ThumbnailPanel(QWidget* parent) : QWidget(parent) {
  cache_.setMaxCost(kCacheBytes / 1024);  // 비용 = KB

  auto* column = new QVBoxLayout(this);
  column->setContentsMargins(0, 0, 0, 0);
  model_ = new ThumbnailModel(this);
  auto* list = new ThumbnailList(this);
  list_ = list;
  list_->setModel(model_);
  list_->setItemDelegate(new ThumbnailDelegate(this));
  list_->setUniformItemSizes(false);
  list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  list_->setSelectionMode(QAbstractItemView::SingleSelection);
  list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  list_->verticalScrollBar()->setSingleStep(24);
  // 흰 쪽이 배경과 구분되도록 목록 배경을 창 색으로
  QPalette pal = list_->palette();
  pal.setColor(QPalette::Base, pal.color(QPalette::Window));
  list_->setPalette(pal);
  column->addWidget(list_);

  request_timer_ = new QTimer(this);
  request_timer_->setSingleShot(true);
  request_timer_->setInterval(0);
  connect(request_timer_, &QTimer::timeout, this, &ThumbnailPanel::request_visible);
  connect(list_->verticalScrollBar(), &QScrollBar::valueChanged, this, &ThumbnailPanel::schedule_request);
  // 스크롤바를 끄는 동안에는 요청하지 않고, 놓으면 그 자리만 요청한다
  connect(list_->verticalScrollBar(), &QScrollBar::sliderReleased, this, &ThumbnailPanel::schedule_request);
  connect(list_, &QListView::clicked, this, &ThumbnailPanel::on_user_activated);
  connect(list_, &QListView::activated, this, &ThumbnailPanel::on_user_activated);
  connect(list_->selectionModel(), &QItemSelectionModel::currentChanged, this,
          [this, list](const QModelIndex& current) {
            if (!syncing_ && list->keyboard_navigating()) on_user_activated(current);  // 키보드 이동
          });
}

ThumbnailPanel::~ThumbnailPanel() { cancel_all(); }

void ThumbnailPanel::set_document(const engine::DocumentHandle& handle, UiDispatcher& dispatcher) {
  cancel_all();
  handle_ = handle;
  dispatcher_ = &dispatcher;
  ++generation_;
  cache_.clear();
  failed_.clear();
  current_page_ = -1;
  update_thumb_width(true);
  {
    const QScopedValueRollback guard(syncing_, true);
    model_->reset_rows(handle_.valid() ? handle_.info().page_count : 0);
  }
  list_->verticalScrollBar()->setValue(0);
  schedule_request();
}

void ThumbnailPanel::clear_document() {
  cancel_all();
  handle_ = engine::DocumentHandle();
  ++generation_;
  cache_.clear();
  failed_.clear();
  current_page_ = -1;
  const QScopedValueRollback guard(syncing_, true);
  model_->reset_rows(0);
}

void ThumbnailPanel::cancel_all() {
  for (auto& [page, pending] : pending_) pending.task.cancel();
  pending_.clear();
}

void ThumbnailPanel::set_current_page(int page) {
  if (page < 0 || page >= model_->rowCount()) return;
  current_page_ = page;
  const QModelIndex index = model_->index(page);
  const QScopedValueRollback guard(syncing_, true);
  list_->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect);
  list_->scrollTo(index, QAbstractItemView::EnsureVisible);
}

void ThumbnailPanel::on_user_activated(const QModelIndex& index) {
  if (!index.isValid()) return;
  emit page_activated(index.row());
}

QImage ThumbnailPanel::thumbnail(int page) const {
  const Entry* entry = cache_.object(page);
  return entry != nullptr ? entry->image : QImage();
}

QSize ThumbnailPanel::thumb_size(int page) const {
  const int width = std::max(thumb_width_, kMinThumb);
  if (!handle_.valid()) return {width, width};
  const auto& sizes = handle_.info().page_sizes;
  if (page < 0 || static_cast<std::size_t>(page) >= sizes.size() || sizes[static_cast<std::size_t>(page)].width <= 0) {
    return {width, width};
  }
  const auto& size = sizes[static_cast<std::size_t>(page)];
  const double aspect = std::clamp(size.height / size.width, 1.0 / kMaxAspect, kMaxAspect);
  return {width, std::max(1, static_cast<int>(std::lround(width * aspect)))};
}

void ThumbnailPanel::update_thumb_width(bool force) {
  const int available = list_->viewport()->width() - 2 * kSideMargin;
  const int width = std::clamp(available, kMinThumb, kMaxThumb);
  if (!force && std::abs(width - thumb_width_) < kWidthStep) return;
  const bool changed = width != thumb_width_;
  thumb_width_ = width;
  if (!changed && !force) return;
  // 대기 중인 이전 폭 요청은 버리고, 캐시된 그림은 새 그림이 올 때까지 늘려 그린다
  cancel_all();
  ++generation_;
  failed_.clear();
  model_->relayout();
  schedule_request();
}

void ThumbnailPanel::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  update_thumb_width(false);
  schedule_request();
}

bool ThumbnailPanel::event(QEvent* event) {
  if (event->type() == QEvent::DevicePixelRatioChange) {
    // 다른 배율의 화면으로 옮겨졌다: 대기 요청은 이전 배율이므로 버리고, 캐시 그림은 새 그림이 올 때까지 늘려 그린다
    cancel_all();
    ++generation_;
    failed_.clear();
    schedule_request();
  }
  return QWidget::event(event);
}

void ThumbnailPanel::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  update_thumb_width(false);
  schedule_request();
}

void ThumbnailPanel::schedule_request() {
  if (!request_timer_->isActive()) request_timer_->start();
}

void ThumbnailPanel::request_visible() {
  const int rows = model_->rowCount();
  if (!handle_.valid() || rows == 0 || dispatcher_ == nullptr || !isVisible()) return;
  if (list_->verticalScrollBar()->isSliderDown()) return;  // 드래그 중: 지나가는 쪽은 요청하지 않는다

  const QRect vp = list_->viewport()->rect();
  QModelIndex first = list_->indexAt(QPoint(vp.center().x(), vp.top() + 1));
  QModelIndex last = list_->indexAt(QPoint(vp.center().x(), vp.bottom() - 1));
  const int first_row = first.isValid() ? first.row() : 0;
  const int last_row = last.isValid() ? last.row() : std::min(rows - 1, first_row + 8);
  const int lo = std::max(0, first_row - kPrefetchRows);
  const int hi = std::min(rows - 1, last_row + kPrefetchRows);

  // 범위를 벗어난 대기 요청은 취소 (콜백은 serial 이 달라 무시된다)
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->first < lo || it->first > hi) {
      it->second.task.cancel();
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }

  // 보이는 행 → 앞뒤 순서로 요청
  std::vector<int> order;
  for (int r = first_row; r <= last_row && r < rows; ++r) order.push_back(r);
  for (int d = 1; d <= kPrefetchRows; ++d) {
    if (last_row + d <= hi) order.push_back(last_row + d);
    if (first_row - d >= lo) order.push_back(first_row - d);
  }

  const auto& sizes = handle_.info().page_sizes;
  const qreal dpr = devicePixelRatioF();
  for (int page : order) {
    if (static_cast<int>(pending_.size()) >= kMaxConcurrent) break;
    if (pending_.contains(page) || failed_.contains(page)) continue;
    const Entry* entry = cache_.object(page);
    if (entry != nullptr && entry->generation == generation_ && entry->dpr == dpr) continue;
    if (static_cast<std::size_t>(page) >= sizes.size()) continue;
    const auto& size = sizes[static_cast<std::size_t>(page)];
    if (size.width <= 0 || size.height <= 0) continue;
    const QSize thumb = thumb_size(page);
    // 쪽 비율을 제한했으면 높이 기준으로 맞춘다
    const double scale = std::min(thumb.width() / size.width, thumb.height() / size.height) * dpr;
    engine::RenderOptions options;
    options.scale = static_cast<float>(scale);
    const int generation = generation_;
    const std::uint64_t serial = ++next_serial_;
    UiDispatcher* dispatcher = dispatcher_;
    QPointer<ThumbnailPanel> self(this);
    engine::TaskHandle task = handle_.render_page(
        page, options, engine::Priority::kLow,
        // 워커 스레드: 메인으로 넘기기만
        [dispatcher, self, page, generation, serial](onpdf::Result<onpdf::Bitmap> result) {
          auto shared = std::make_shared<onpdf::Result<onpdf::Bitmap>>(std::move(result));
          dispatcher->post_to(self, [page, generation, serial, shared](ThumbnailPanel* panel) {
            panel->on_rendered(page, generation, serial, std::move(*shared));
          });
        });
    pending_[page] = Pending{std::move(task), serial};
    ++requested_total_;
  }
}

void ThumbnailPanel::on_rendered(int page, int generation, std::uint64_t serial,
                                 onpdf::Result<onpdf::Bitmap> result) {
  if (generation != generation_) return;  // 다른 문서·이전 폭
  auto it = pending_.find(page);
  if (it == pending_.end() || it->second.serial != serial) return;  // 취소 후 재요청된 쪽
  pending_.erase(it);
  if (!result) {
    if (result.error().code != ErrorCode::kCancelled) failed_.insert(page);  // 다시 시도하지 않는다
    schedule_request();
    return;
  }
  auto entry = std::make_unique<Entry>();
  entry->image = to_qimage(std::move(*result), devicePixelRatioF());
  entry->generation = generation;
  entry->dpr = devicePixelRatioF();
  const auto bytes = entry->image.sizeInBytes();
  const int cost = std::max(1, static_cast<int>(bytes / 1024));
  cache_.insert(page, entry.release(), cost);
  model_->page_changed(page);
  schedule_request();
}

}  // namespace onpdf::app
