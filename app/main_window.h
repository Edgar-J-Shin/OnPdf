#pragma once

#include "document_session.h"
#include "edit_controller.h"
#include "print_job.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/view/search.h"
#include "onpdf/core/view/text_index.h"

#include <QMainWindow>
#include <QString>

#include <functional>
#include <memory>
#include <string_view>

class QCloseEvent;
class QLabel;
class QPrinter;
class QProgressDialog;
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
  [[nodiscard]] QAction* print_action() const noexcept { return print_action_; }
  [[nodiscard]] PrintJob* print_job() const noexcept { return print_job_.get(); }
  [[nodiscard]] engine::TaskHandle outline_task() const { return outline_task_; }  // 테스트용
  [[nodiscard]] EditController* edit_controller() const noexcept { return edit_; }
  [[nodiscard]] QAction* save_action() const noexcept { return save_action_; }
  [[nodiscard]] QAction* undo_action() const noexcept { return undo_action_; }
  [[nodiscard]] QAction* redo_action() const noexcept { return redo_action_; }
  [[nodiscard]] QAction* rotate_cw_action() const noexcept { return rotate_cw_action_; }
  [[nodiscard]] bool saving() const noexcept { return saving_; }
  // 현재 쪽 회전 (시계 방향 quarter_turns). 편집 메뉴와 같은 경로
  void rotate_current_page(int quarter_turns);
  // 이 경로로 저장한다 (대화상자 없이 — 다른 이름으로 저장의 뒷부분, 테스트용). 시작하면 true
  bool save_to(const QString& path);
  // 대화상자 없이 이 프린터 설정으로 인쇄를 시작한다 (파일 > 인쇄 의 뒷부분, 테스트용). 시작하면 true
  bool print_with(QPrinter& printer);
  // 첫 쪽이 그려지면 창을 PNG 로 저장하고 앱을 종료한다 (QA·CI 스크린샷용, --screenshot 옵션).
  void screenshot_after_render(const QString& png_path);

 protected:
  void closeEvent(QCloseEvent* event) override;
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
  // ---- 편집·저장 ----
  void update_edit_actions();
  void update_title();
  void on_document_changed(const onpdf::engine::ChangeSet& change);
  void on_save();
  void on_save_as();
  // 저장을 시작한다. 성공하면 then 을 부른다 (실패하면 안내만)
  bool start_save(const QString& path, std::function<void()> then);
  // 수정된 문서면 [저장][저장 안 함][취소] 를 묻는다. 계속해도 되면 then 을 (저장 성공 뒤에) 부른다
  void maybe_save(std::function<void()> then);
  void continue_open(const QString& path);
  [[nodiscard]] QString command_label(std::string_view id) const;
  void on_print();
  void on_print_finished(PrintJob::Outcome outcome);
  void cancel_print();
  // 인쇄 중 새 문서를 열려 할 때 확인. "인쇄 취소하고 열기" 면 인쇄를 취소하고 true
  bool confirm_cancel_print_for(const QString& path);
  void discard_print_job();  // 진행 중이던 인쇄를 조용히 버리고 진행 창을 닫는다 (새 문서가 열렸을 때)
  void update_print_action();
  void on_outline_item(const onpdf::view::Link& target);
  // 쪽 대상으로 이동: location(링크)이면 바로, user_location(목차)이면 워커에서 변환 후, 둘 다 없으면 쪽 맨 위
  void navigate_to(const onpdf::view::Link& target);
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
  QAction* print_action_ = nullptr;
  QAction* save_action_ = nullptr;
  QAction* save_as_action_ = nullptr;
  QAction* undo_action_ = nullptr;
  QAction* redo_action_ = nullptr;
  QAction* rotate_cw_action_ = nullptr;
  QAction* rotate_ccw_action_ = nullptr;
  EditController* edit_ = nullptr;
  bool saving_ = false;
  bool allow_close_ = false;  // 저장 확인을 거쳐 닫기로 했다
  QProgressDialog* print_progress_ = nullptr;
  bool print_prefs_applied_ = false;  // 문서마다 처음 인쇄할 때 문서 인쇄 기본값을 대화상자에 넣는다
  QTimer* outline_highlight_timer_ = nullptr;
  QTimer* status_timer_ = nullptr;
  engine::TaskHandle outline_task_;  // 진행 중인 목차 로드 (문서 교체 시 취소)
  int outline_generation_ = 0;  // 문서를 열 때마다 증가. 늦게 온 목차·위치 결과를 버린다
  std::unique_ptr<view::SearchSession> search_;
  int search_scroll_anchor_ = 0;  // 검색 시작 시 스크롤 위치. 사용자가 움직였으면 첫 결과로 끌고 가지 않는다
  QString screenshot_path_;
  // 선언 순서 중요: print_job_ 이 printer_ 를 참조하므로 printer_ 가 먼저 선언되어 나중에 소멸한다
  std::unique_ptr<QPrinter> printer_;
  std::unique_ptr<PrintJob> print_job_;
};

}  // namespace onpdf::app
