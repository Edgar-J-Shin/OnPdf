#include "outline_panel.h"

#include <QHeaderView>
#include <QLabel>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTreeView>
#include <QVBoxLayout>

#include <cstddef>
#include <utility>

namespace onpdf::app {
namespace {

constexpr int kItemRole = Qt::UserRole + 1;  // Outline::items 인덱스

std::size_t to_size(int value) { return static_cast<std::size_t>(value); }

}  // namespace

OutlinePanel::OutlinePanel(QWidget* parent) : QWidget(parent) {
  auto* column = new QVBoxLayout(this);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(0);

  stack_ = new QStackedWidget(this);
  tree_ = new QTreeView(stack_);
  model_ = new QStandardItemModel(this);
  tree_->setModel(model_);
  tree_->setHeaderHidden(true);
  tree_->setIndentation(16);  // 기본 20px 보다 좁혀 폭 200px 사이드바에서 더 깊은 단계까지 제목이 보이게
  tree_->setUniformRowHeights(true);
  tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  tree_->setSelectionMode(QAbstractItemView::SingleSelection);
  tree_->setTextElideMode(Qt::ElideRight);
  tree_->header()->setStretchLastSection(true);

  empty_label_ = new QLabel(tr("목차가 없습니다"), stack_);
  empty_label_->setAlignment(Qt::AlignCenter);
  empty_label_->setWordWrap(true);
  empty_label_->setEnabled(false);  // 흐린 글씨

  stack_->addWidget(tree_);
  stack_->addWidget(empty_label_);
  stack_->setCurrentWidget(empty_label_);

  notice_label_ = new QLabel(tr("목차가 손상되었거나 너무 커서 일부만 표시합니다"), this);
  notice_label_->setWordWrap(true);
  notice_label_->setContentsMargins(6, 4, 6, 4);
  notice_label_->hide();

  column->addWidget(stack_, 1);
  column->addWidget(notice_label_);

  connect(tree_, &QTreeView::clicked, this, &OutlinePanel::on_index_activated);
  connect(tree_, &QTreeView::activated, this, &OutlinePanel::on_index_activated);
}

OutlinePanel::~OutlinePanel() = default;

void OutlinePanel::set_outline(std::shared_ptr<const view::Outline> outline) {
  outline_ = std::move(outline);
  highlighted_ = -1;
  rows_.clear();
  model_->clear();

  const bool has_items = outline_ && !outline_->empty();
  notice_label_->setVisible(outline_ && (outline_->truncated || outline_->cycle_detected));
  if (!has_items) {
    stack_->setCurrentWidget(empty_label_);
    return;
  }

  rows_.assign(outline_->items.size(), nullptr);
  QStandardItem* root = model_->invisibleRootItem();
  // 전위 순서라 부모가 항상 먼저 만들어진다
  for (int i = 0; i < outline_->size(); ++i) {
    const auto& item = outline_->items[to_size(i)];
    QString title = QString::fromUtf8(item.title.data(), static_cast<qsizetype>(item.title.size())).simplified();
    if (title.isEmpty()) title = tr("(제목 없음)");
    auto* row = new QStandardItem(title);
    row->setData(i, kItemRole);
    row->setToolTip(title);
    if (!item.target) row->setForeground(palette().brush(QPalette::Disabled, QPalette::Text));
    QStandardItem* parent = item.parent >= 0 && item.parent < i ? rows_[to_size(item.parent)] : root;
    if (parent == nullptr) parent = root;
    parent->appendRow(row);
    rows_[to_size(i)] = row;
  }
  // /Count > 0 인 항목만 펼친다 (문서 작성자가 의도한 초기 상태)
  for (int i = 0; i < outline_->size(); ++i) {
    if (outline_->items[to_size(i)].open) tree_->setExpanded(rows_[to_size(i)]->index(), true);
  }
  stack_->setCurrentWidget(tree_);
}

int OutlinePanel::row_count() const noexcept { return static_cast<int>(rows_.size()); }

void OutlinePanel::highlight_for(int page, double y_pt) {
  if (!outline_ || outline_->empty() || page < 0) return;
  int index = outline_->item_at(page, y_pt);
  // 같은 위치를 가리키는 항목이 여럿이면(예: 장과 첫 절이 같은 쪽 맨 위) 사용자가 고른 항목을 유지한다
  if (index >= 0 && highlighted_ >= 0 && highlighted_ != index) {
    const auto& now = outline_->items[to_size(highlighted_)];
    const auto& found = outline_->items[to_size(index)];
    if (now.target && found.target && now.target->kind == view::LinkKind::kPage &&
        found.target->kind == view::LinkKind::kPage && now.target->page == found.target->page &&
        now.approx_y == found.approx_y) {
      index = highlighted_;
    }
  }
  if (index == highlighted_) return;
  // 사용자가 트리를 만지는 중(포인터가 위에 있음)이면 선택만 바꾸고 스크롤로 끌고 가지 않는다
  select_item(index, !tree_->underMouse());
}

void OutlinePanel::select_item(int item_index, bool scroll) {
  highlighted_ = item_index;
  const QSignalBlocker blocker(tree_->selectionModel());
  if (item_index < 0 || to_size(item_index) >= rows_.size()) {
    tree_->selectionModel()->clearSelection();
    tree_->viewport()->update();
    return;
  }
  const QModelIndex index = rows_[to_size(item_index)]->index();
  tree_->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect);
  // QTreeView::scrollTo 는 접힌 조상을 펼쳐 항목을 보이게 한다
  if (scroll) tree_->scrollTo(index, QAbstractItemView::EnsureVisible);
  tree_->viewport()->update();
}

void OutlinePanel::activate_item(int item_index) {
  if (item_index < 0 || to_size(item_index) >= rows_.size()) return;
  on_index_activated(rows_[to_size(item_index)]->index());
}

void OutlinePanel::on_index_activated(const QModelIndex& index) {
  if (!outline_ || !index.isValid()) return;
  const int item_index = index.data(kItemRole).toInt();
  if (item_index < 0 || item_index >= outline_->size()) return;
  highlighted_ = item_index;
  const auto& item = outline_->items[to_size(item_index)];
  if (item.target) emit item_activated(*item.target);
}

}  // namespace onpdf::app
