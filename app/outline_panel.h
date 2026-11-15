#pragma once

#include "onpdf/core/view/outline.h"
#include "onpdf/core/view/text_index.h"

#include <QWidget>

#include <memory>
#include <vector>

class QLabel;
class QModelIndex;
class QStackedWidget;
class QStandardItem;
class QStandardItemModel;
class QTreeView;

namespace onpdf::app {

// 문서 목차 트리. 목차 스냅샷(shared_ptr<const Outline>)을 받아 표시만 한다 (PDFium 접근 없음).
class OutlinePanel : public QWidget {
  Q_OBJECT
 public:
  explicit OutlinePanel(QWidget* parent = nullptr);
  ~OutlinePanel() override;

  // nullptr·빈 목차 → "목차가 없습니다". 손상(순환·깊이·개수 제한) 시 안내 문구를 함께 보인다.
  void set_outline(std::shared_ptr<const view::Outline> outline);
  [[nodiscard]] const view::Outline* outline() const noexcept { return outline_.get(); }
  // 문서 위치(쪽, 쪽 포인트 y) 에 해당하는 항목을 강조한다. 사용자가 트리를 조작 중이면 스크롤하지 않는다.
  void highlight_for(int page, double y_pt);
  // 테스트용
  [[nodiscard]] int row_count() const noexcept;           // 전체 항목 수
  [[nodiscard]] int highlighted_item() const noexcept { return highlighted_; }  // Outline::items 인덱스, 없으면 -1
  void activate_item(int item_index);                     // 클릭과 같은 동작
  [[nodiscard]] QTreeView* tree() const noexcept { return tree_; }

 signals:
  void item_activated(const onpdf::view::Link& target);

 private:
  void on_index_activated(const QModelIndex& index);
  void select_item(int item_index, bool scroll);

  std::shared_ptr<const view::Outline> outline_;
  std::vector<QStandardItem*> rows_;  // Outline::items 인덱스 → 모델 항목 (모델 소유)
  int highlighted_ = -1;
  QStackedWidget* stack_ = nullptr;
  QTreeView* tree_ = nullptr;
  QStandardItemModel* model_ = nullptr;
  QLabel* empty_label_ = nullptr;
  QLabel* notice_label_ = nullptr;
};

}  // namespace onpdf::app
