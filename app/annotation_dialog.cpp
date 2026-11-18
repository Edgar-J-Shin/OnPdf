#include "annotation_dialog.h"

#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace onpdf::app {

AnnotationDialog::AnnotationDialog(const QString& title, const QString& text, QWidget* parent) : QDialog(parent) {
  setObjectName(QStringLiteral("note_editor"));
  setWindowTitle(title);
  setModal(true);
  resize(360, 220);
  auto* column = new QVBoxLayout(this);
  edit_ = new QPlainTextEdit(this);
  edit_->setObjectName(QStringLiteral("note_text"));
  edit_->setPlainText(text);
  edit_->setPlaceholderText(tr("메모 내용"));
  column->addWidget(edit_, 1);
  auto* row = new QHBoxLayout();
  row->addStretch(1);
  auto* ok = new QPushButton(tr("확인"), this);
  ok->setDefault(true);
  auto* cancel = new QPushButton(tr("취소"), this);
  row->addWidget(ok);
  row->addWidget(cancel);
  column->addLayout(row);
  connect(ok, &QPushButton::clicked, this, &QDialog::accept);
  connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
  edit_->setFocus();
}

QString AnnotationDialog::text() const { return edit_->toPlainText(); }

}  // namespace onpdf::app
