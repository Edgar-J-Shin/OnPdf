#pragma once

#include "document_session.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/view/text_index.h"

#include <QMainWindow>
#include <QString>

class QLabel;
class QSpinBox;

namespace onpdf::app {

class DocumentView;

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
  void on_first_view_rendered();
  void on_copy();
  void on_select_all();
  void on_link_activated(const onpdf::view::Link& link);
  void on_link_hovered(const QString& target);
  void on_current_page_changed(int page);
  void on_zoom_changed(double zoom);
  void refresh_status();
  void set_document_status(const QString& text);   // 문서 정보 (지속)
  void show_transient(const QString& text);        // 진행·오류 안내 (몇 초 뒤 사라짐)
  void fail_screenshot(const QString& reason);     // --screenshot 모드에서 실패 시 종료

  UiDispatcher& dispatcher_;
  DocumentSession session_;
  DocumentView* view_ = nullptr;
  QSpinBox* page_spin_ = nullptr;
  QLabel* page_total_label_ = nullptr;
  QLabel* status_label_ = nullptr;
  QLabel* link_label_ = nullptr;
  QAction* copy_action_ = nullptr;
  QAction* select_all_action_ = nullptr;
  QString screenshot_path_;
};

}  // namespace onpdf::app
