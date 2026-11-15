#pragma once

// 문서 위쪽 검색 바. 상태를 갖지 않는 위젯: 입력·옵션을 시그널로 내보내고, 결과 표시는 set_status 로 받는다.
// 입력은 300ms 디바운스 후 query_changed. Enter 다음, Shift+Enter 이전, Esc 닫기.

#include "onpdf/core/view/search.h"

#include <QString>
#include <QTimer>
#include <QWidget>

#include <cstddef>

class QLabel;
class QLineEdit;
class QToolButton;

namespace onpdf::app {

class SearchBar : public QWidget {
  Q_OBJECT
 public:
  explicit SearchBar(QWidget* parent = nullptr);

  // 표시 + 입력창 포커스·전체 선택. initial 이 비어 있지 않으면 입력창을 그 값으로
  void open_with(const QString& initial);
  void set_status(std::size_t ordinal, std::size_t total, bool searching, bool truncated, bool no_text);
  [[nodiscard]] QString query() const;
  [[nodiscard]] view::SearchOptions options() const;
  [[nodiscard]] QLineEdit* line_edit() const noexcept { return edit_; }
  [[nodiscard]] QString status_text() const;

  static constexpr int kDebounceMs = 300;

 signals:
  void query_changed(const QString& query, onpdf::view::SearchOptions options);
  void next_requested();
  void previous_requested();
  void closed();

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void emit_query();
  void set_no_match(bool no_match);
  void close_bar();

  QLineEdit* edit_ = nullptr;
  QLabel* status_ = nullptr;
  QToolButton* prev_ = nullptr;
  QToolButton* next_ = nullptr;
  QToolButton* case_ = nullptr;
  QToolButton* word_ = nullptr;
  QToolButton* close_ = nullptr;
  QTimer debounce_;
};

}  // namespace onpdf::app
