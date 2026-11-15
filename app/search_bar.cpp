#include "search_bar.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>

namespace onpdf::app {

SearchBar::SearchBar(QWidget* parent) : QWidget(parent) {
  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(8, 4, 8, 4);
  layout->setSpacing(4);

  edit_ = new QLineEdit(this);
  edit_->setPlaceholderText(tr("문서에서 찾기"));
  edit_->setClearButtonEnabled(true);
  edit_->setMinimumWidth(220);
  edit_->installEventFilter(this);
  status_ = new QLabel(this);
  status_->setMinimumWidth(110);
  auto make_button = [this](const QString& text, const QString& tip, bool checkable = false) {
    auto* b = new QToolButton(this);
    b->setText(text);
    b->setToolTip(tip);
    b->setCheckable(checkable);
    b->setAutoRaise(true);
    return b;
  };
  prev_ = make_button(QStringLiteral("‹"), tr("이전 결과 (Shift+Enter)"));
  next_ = make_button(QStringLiteral("›"), tr("다음 결과 (Enter)"));
  case_ = make_button(QStringLiteral("Aa"), tr("대소문자 구분"), true);
  word_ = make_button(tr("단어"), tr("단어 단위로 찾기"), true);
  close_ = make_button(QStringLiteral("✕"), tr("닫기 (Esc)"));

  layout->addWidget(new QLabel(tr("찾기"), this));
  layout->addWidget(edit_, 1);
  layout->addWidget(status_);
  layout->addWidget(prev_);
  layout->addWidget(next_);
  layout->addWidget(case_);
  layout->addWidget(word_);
  layout->addWidget(close_);

  debounce_.setSingleShot(true);
  debounce_.setInterval(kDebounceMs);
  connect(&debounce_, &QTimer::timeout, this, &SearchBar::emit_query);
  // 확정 텍스트만 쓴다: IME 조합 중인 글자는 QLineEdit::text() 에 들어오지 않는다
  connect(edit_, &QLineEdit::textChanged, this, [this] { debounce_.start(); });
  connect(case_, &QToolButton::toggled, this, &SearchBar::emit_query);
  connect(word_, &QToolButton::toggled, this, &SearchBar::emit_query);
  connect(prev_, &QToolButton::clicked, this, &SearchBar::previous_requested);
  connect(next_, &QToolButton::clicked, this, &SearchBar::next_requested);
  connect(close_, &QToolButton::clicked, this, [this] {
    hide();
    emit closed();
  });
  hide();
}

void SearchBar::open_with(const QString& initial) {
  show();
  if (!initial.isEmpty() && initial != edit_->text()) {
    edit_->setText(initial);  // 디바운스 후 검색
  }
  edit_->setFocus(Qt::ShortcutFocusReason);
  edit_->selectAll();
}

QString SearchBar::query() const { return edit_->text(); }

view::SearchOptions SearchBar::options() const {
  view::SearchOptions o;
  o.match_case = case_->isChecked();
  o.whole_word = word_->isChecked();
  return o;
}

QString SearchBar::status_text() const { return status_->text(); }

void SearchBar::emit_query() {
  debounce_.stop();
  emit query_changed(edit_->text(), options());
}

void SearchBar::set_no_match(bool no_match) {
  edit_->setStyleSheet(no_match ? QStringLiteral("QLineEdit { background: #ffe0e0; color: #000; }") : QString());
}

void SearchBar::set_status(std::size_t ordinal, std::size_t total, bool searching, bool truncated, bool no_text) {
  if (edit_->text().trimmed().isEmpty()) {
    status_->clear();
    set_no_match(false);
    return;
  }
  if (no_text) {
    status_->setText(tr("텍스트 없음"));
    status_->setToolTip(tr("이 문서에는 검색할 텍스트가 없습니다 (스캔본일 수 있음)"));
    set_no_match(true);
    return;
  }
  status_->setToolTip({});
  if (total == 0) {
    status_->setText(searching ? tr("검색 중…") : tr("결과 없음"));
    set_no_match(!searching);
    return;
  }
  set_no_match(false);
  QString text = QStringLiteral("%1 / %2").arg(ordinal).arg(total);
  if (truncated) {
    text += tr("+ (너무 많음)");
  } else if (searching) {
    text += QStringLiteral("+");
  }
  status_->setText(text);
}

bool SearchBar::eventFilter(QObject* watched, QEvent* event) {
  if (watched == edit_ && event->type() == QEvent::KeyPress) {
    auto* key = static_cast<QKeyEvent*>(event);
    if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
      if (debounce_.isActive()) emit_query();  // 디바운스 대기 중이면 먼저 검색
      if (key->modifiers() & Qt::ShiftModifier) {
        emit previous_requested();
      } else {
        emit next_requested();
      }
      return true;
    }
    if (key->key() == Qt::Key_Escape) {
      hide();
      emit closed();
      return true;
    }
  }
  return QWidget::eventFilter(watched, event);
}

}  // namespace onpdf::app
