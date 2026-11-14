#pragma once

#include "document_session.h"
#include "onpdf/core/engine/pdfium_worker.h"

#include <QMainWindow>
#include <QString>

class QLabel;

namespace onpdf::app {

class PageView;

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  MainWindow(engine::PdfiumWorker& worker, UiDispatcher& dispatcher, QWidget* parent = nullptr);

  void open_file(const QString& path);
  // 첫 쪽이 그려지면 창을 PNG 로 저장하고 앱을 종료한다 (QA·CI 스크린샷용, --screenshot 옵션).
  void screenshot_after_render(const QString& png_path);

 protected:
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;

 private:
  void build_menus();
  void on_open_action();
  void on_opened(const QString& path, int page_count);
  void on_password_required(const QString& path);
  void on_open_failed(const QString& path, const QString& message);
  void on_page_rendered(int page_index, const QImage& image, int elapsed_ms);
  void on_render_failed(int page_index, const QString& message);
  void request_render();
  void set_status(const QString& text);

  DocumentSession session_;
  PageView* page_view_ = nullptr;
  QLabel* status_label_ = nullptr;
  int current_page_ = 0;
  QString screenshot_path_;
};

}  // namespace onpdf::app
