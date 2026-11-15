#pragma once

#include "document_session.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/view/search.h"
#include "onpdf/core/view/text_index.h"

#include <QMainWindow>
#include <QString>

#include <memory>

class QLabel;
class QSpinBox;
class QSplitter;
class QTabWidget;
class QTimer;

namespace onpdf::app {

class DocumentView;
class OutlinePanel;
class SearchBar;
class ThumbnailPanel;

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  MainWindow(engine::PdfiumWorker& worker, UiDispatcher& dispatcher, QWidget* parent = nullptr);
  ~MainWindow() override;

  void open_file(const QString& path);
  // 검색 바를 연다 (편집 > 찾기). 선택이 한 줄·100자 이하면 검색어로
  void open_find();
  // 테스트용 접근자
  [[nodiscard]] DocumentView* document_view() const noexcept { return view_; }
  [[nodiscard]] SearchBar* search_bar() const noexcept { return search_bar_; }
  [[nodiscard]] const view::SearchSession* search_session() const noexcept { return search_.get(); }
  [[nodiscard]] QTabWidget* sidebar() const noexcept { return sidebar_; }
  [[nodiscard]] OutlinePanel* outline_panel() const noexcept { return outline_panel_; }
  [[nodiscard]] ThumbnailPanel* thumbnail_panel() const noexcept { return thumbnail_panel_; }
  [[nodiscard]] QAction* sidebar_action() const noexcept { return sidebar_action_; }
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
  void reset_search();
  void on_search_query(const QString& query, onpdf::view::SearchOptions options);
  void on_find_next();
  void on_find_previous();
  void on_search_closed();
  void update_search_status();
  void on_current_page_changed(int page);
  void load_outline();
  void on_outline_item(const onpdf::view::Link& target);
  void update_outline_highlight();
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
  QAction* find_next_action_ = nullptr;
  QAction* find_previous_action_ = nullptr;
  SearchBar* search_bar_ = nullptr;
  QSplitter* splitter_ = nullptr;
  QTabWidget* sidebar_ = nullptr;
  ThumbnailPanel* thumbnail_panel_ = nullptr;
  OutlinePanel* outline_panel_ = nullptr;
  QAction* sidebar_action_ = nullptr;
  QTimer* outline_highlight_timer_ = nullptr;
  int outline_generation_ = 0;  // 문서를 열 때마다 증가. 늦게 온 목차·위치 결과를 버린다
  std::unique_ptr<view::SearchSession> search_;
  int search_scroll_anchor_ = 0;  // 검색 시작 시 스크롤 위치. 사용자가 움직였으면 첫 결과로 끌고 가지 않는다
  QString screenshot_path_;
};

}  // namespace onpdf::app
