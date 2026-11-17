#pragma once

// 메모 내용 편집 창. Popup 주석은 PDFium 이 그리지 않으므로 내용은 이 창(과 툴팁)으로 보인다

#include <QDialog>
#include <QString>

class QPlainTextEdit;

namespace onpdf::app {

class AnnotationDialog : public QDialog {
  Q_OBJECT
 public:
  AnnotationDialog(const QString& title, const QString& text, QWidget* parent = nullptr);
  [[nodiscard]] QString text() const;

 private:
  QPlainTextEdit* edit_ = nullptr;
};

}  // namespace onpdf::app
