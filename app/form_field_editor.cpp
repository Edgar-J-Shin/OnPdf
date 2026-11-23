#include "form_field_editor.h"

#include <QFocusEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <algorithm>

namespace onpdf::app {

FormFieldEditor::FormFieldEditor(int page, engine::FormWidgetInfo widget, QWidget* parent)
    : QFrame(parent), page_(page), widget_(std::move(widget)) {
  setObjectName(QStringLiteral("form_field_editor"));
  setFrameShape(QFrame::NoFrame);
  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  const QString value = QString::fromStdString(widget_.value);
  if (widget_.multiline) {
    multi_ = new QPlainTextEdit(this);
    multi_->setObjectName(QStringLiteral("form_text_multi"));
    multi_->setPlainText(value);
    multi_->setFrameShape(QFrame::Box);
    multi_->installEventFilter(this);
    layout->addWidget(multi_);
  } else {
    line_ = new QLineEdit(this);
    line_->setObjectName(QStringLiteral("form_text_line"));
    line_->setText(value);
    line_->selectAll();
    if (widget_.password) line_->setEchoMode(QLineEdit::Password);
    if (widget_.max_len > 0) line_->setMaxLength(widget_.max_len);
    line_->installEventFilter(this);
    layout->addWidget(line_);
  }
}

void FormFieldEditor::place(const QRectF& viewport_rect, double px_per_pt) {
  // 너무 작은 칸(체크 크기)도 입력할 수 있게 최소 크기를 둔다
  const QRectF r = viewport_rect.adjusted(-1, -1, 1, 1);
  setGeometry(QRect(r.topLeft().toPoint(),
                    QSize(std::max(40, qRound(r.width())), std::max(18, qRound(r.height())))));
  double pixel =
      widget_.font_size > 0 ? widget_.font_size * px_per_pt : viewport_rect.height() * 0.6;
  if (widget_.multiline && widget_.font_size <= 0) pixel = 12.0 * px_per_pt;
  QFont font = this->font();
  font.setPixelSize(std::max(8, qRound(pixel)));
  if (line_) line_->setFont(font);
  if (multi_) multi_->setFont(font);
}

void FormFieldEditor::focus_editor() {
  if (line_) line_->setFocus(Qt::OtherFocusReason);
  if (multi_) multi_->setFocus(Qt::OtherFocusReason);
}

QString FormFieldEditor::text() const {
  if (line_) return line_->text();
  if (multi_) return multi_->toPlainText();
  return {};
}

void FormFieldEditor::set_text(const QString& text) {
  if (line_) line_->setText(text);
  if (multi_) multi_->setPlainText(text);
}

void FormFieldEditor::commit(int advance) {
  if (finished_) return;
  finished_ = true;
  emit committed(advance);
}

void FormFieldEditor::cancel() {
  if (finished_) return;
  finished_ = true;
  emit cancelled();
}

bool FormFieldEditor::eventFilter(QObject* watched, QEvent* event) {
  if (watched != line_ && watched != multi_) return QFrame::eventFilter(watched, event);
  if (event->type() == QEvent::KeyPress) {
    auto* key = static_cast<QKeyEvent*>(event);
    if (key->key() == Qt::Key_Escape) {
      cancel();
      return true;
    }
    if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
      commit(key->key() == Qt::Key_Backtab || (key->modifiers() & Qt::ShiftModifier) ? -1 : 1);
      return true;
    }
    if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
      // 여러 줄은 Enter 가 줄바꿈, Ctrl/Cmd+Enter 가 확정
      if (!multi_ || (key->modifiers() & Qt::ControlModifier)) {
        commit(0);
        return true;
      }
    }
  }
  if (event->type() == QEvent::FocusOut) {
    // 입력기 후보 창·메뉴로 잠깐 나가는 경우는 확정하지 않는다
    const auto reason = static_cast<QFocusEvent*>(event)->reason();
    if (reason != Qt::PopupFocusReason && reason != Qt::ActiveWindowFocusReason) commit(0);
  }
  return QFrame::eventFilter(watched, event);
}

}  // namespace onpdf::app
