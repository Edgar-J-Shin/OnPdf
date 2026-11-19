#pragma once

#include "document_session.h"
#include "document_view.h"
#include "edit_controller.h"
#include "structure_job.h"
#include "print_job.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/view/search.h"
#include "onpdf/core/view/text_index.h"

#include <QColor>
#include <QMainWindow>
#include <QString>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

class QCloseEvent;
class QLabel;
class QPrinter;
class QToolBar;
class QActionGroup;
class QToolButton;
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
  [[nodiscard]] QAction* delete_pages_action() const noexcept { return delete_pages_action_; }
  [[nodiscard]] QAction* merge_action() const noexcept { return merge_action_; }
  [[nodiscard]] QAction* split_action() const noexcept { return split_action_; }
  [[nodiscard]] StructureJob* structure_job() const noexcept { return structure_job_; }
  [[nodiscard]] QAction* tool_action(AnnotationTool tool) const;
  [[nodiscard]] QAction* delete_annotation_action() const noexcept { return delete_annotation_action_; }
  [[nodiscard]] QToolBar* annotation_toolbar() const noexcept { return annotation_toolbar_; }
  [[nodiscard]] engine::Color annotation_color() const noexcept { return annotation_color_; }
  [[nodiscard]] QAction* move_up_action() const noexcept { return move_up_action_; }
  [[nodiscard]] QAction* move_down_action() const noexcept { return move_down_action_; }
  [[nodiscard]] QAction* insert_pages_action() const noexcept { return insert_pages_action_; }
  [[nodiscard]] bool saving() const noexcept { return saving_; }
  // 현재 쪽(썸네일에 선택이 있으면 선택 쪽들) 회전 (시계 방향 quarter_turns). 쪽 메뉴와 같은 경로
  void rotate_current_page(int quarter_turns);
  // ---- 쪽 구조 편집 (쪽 메뉴·썸네일과 같은 경로, 테스트용으로도 쓴다). 시작하면 true ----
  bool rotate_pages(std::vector<int> pages, int quarter_turns);
  bool delete_pages(std::vector<int> pages);
  // pages 를 썸네일 행 drop_row(0..쪽 수) 자리로. 자리가 그대로면 아무것도 하지 않고 false
  bool move_pages(std::vector<int> pages, int drop_row);
  // path 의 PDF 전체를 at 자리에 삽입. 암호가 필요하면 묻고 다시 시도한다
  bool insert_pages_from(const QString& path, int at, const QString& password = {});
  // 쪽 메뉴·단축키의 대상: 썸네일 목록에 포커스와 선택이 있으면 선택, 아니면 현재 쪽
  [[nodiscard]] std::vector<int> target_pages() const;
  // ---- 주석 (툴바·메뉴·뷰 신호와 같은 경로, 테스트용으로도 쓴다). 시작하면 true ----
  void set_annotation_color(engine::Color color);  // 새 주석 색. 주석이 선택돼 있으면 그 주석의 색을 바꾼다
  bool add_markup_from_selection(engine::AnnotationKind kind);
  bool delete_selected_annotation();
  // 메모 생성: contents 가 주어지면 대화상자 없이 만든다 (테스트용)
  bool add_note(int page, onpdf::Point pt, const QString& contents);
  // ---- 병합·분할 (대화상자 없이 같은 경로를 타는 진입점, 테스트용으로도 쓴다). 시작하면 true ----
  bool merge_files(std::vector<engine::StructureInput> inputs, const QString& output);
  bool split_document(int every, const QString& directory);  // 현재 문서(디스크 파일)를 every 쪽마다
  void session_open_for_test(const QString& path, const QString& password) { session_.open(path, password); }  // 암호 대화상자 우회
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
  void load_outline(bool switch_tab);  // switch_tab: 목차가 있으면 목차 탭으로 (문서를 열 때만)
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
  void on_edit_failed(onpdf::ErrorCode code, const QString& message);
  void build_annotation_ui();
  void on_markup_requested(onpdf::engine::AnnotationKind kind, const MarkupItems& items);
  void on_note_requested(int page, onpdf::Point pt);
  void on_ink_requested(int page, const std::vector<onpdf::Point>& points, onpdf::Rect bbox);
  void on_annotation_selected(bool has, const onpdf::engine::AnnotationInfo& info);
  void on_annotation_edit_requested(int page, const onpdf::engine::AnnotationInfo& info);
  [[nodiscard]] std::string annotation_author() const;
  void update_color_icon();
  void on_insert_pages_action();
  void on_merge_action();
  void on_split_action();
  void prompt_split();
  void on_structure_finished(bool cancelled);
  void show_structure_progress(const QString& title);
  void move_selection(int delta);  // 위로(-1)·아래로(+1) 이동
  bool warn_signature_once();      // 서명 문서의 첫 구조 편집: 계속하면 true
  void on_thumbnail_context_menu(const QPoint& global_pos, const std::vector<int>& pages);
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
  QAction* delete_pages_action_ = nullptr;
  QAction* move_up_action_ = nullptr;
  QAction* move_down_action_ = nullptr;
  QAction* insert_pages_action_ = nullptr;
  QAction* merge_action_ = nullptr;
  QAction* split_action_ = nullptr;
  StructureJob* structure_job_ = nullptr;
  QProgressDialog* structure_progress_ = nullptr;
  std::vector<std::filesystem::path> structure_outputs_;  // 진행 중 작업의 출력 (취소·실패 시 정리, 성공 시 열기)
  enum class StructureKind { kNone, kMerge, kSplit } structure_kind_ = StructureKind::kNone;
  QString structure_folder_;
  std::vector<engine::StructureInput> pending_merge_inputs_;  // 암호를 물어 다시 시도할 병합
  QString pending_merge_output_;
  int merge_password_attempts_ = 0;
  QToolBar* annotation_toolbar_ = nullptr;
  QActionGroup* tool_group_ = nullptr;
  QAction* tool_actions_[6] = {};  // AnnotationTool 순서
  QToolButton* color_button_ = nullptr;
  QAction* delete_annotation_action_ = nullptr;
  QAction* markup_selection_actions_[3] = {};  // 하이라이트·밑줄·취소선 (선택 영역)
  engine::Color annotation_color_{255, 255, 0, 255};
  bool annotation_edits_allowed_ = false;  // idle && 권한. 도구 선택은 권한만 보고, 실제 편집 요청은 이것으로 막는다
  bool updating_actions_ = false;          // update_edit_actions 재진입 차단
  struct PendingInsert {
    QString path;
    int at = 0;
  };
  std::optional<PendingInsert> pending_insert_;  // 암호가 필요하면 물은 뒤 같은 삽입을 다시 시도한다
  bool signature_warned_ = false;                // 서명 문서 구조 편집 경고는 문서마다 한 번
  EditController* edit_ = nullptr;
  bool saving_ = false;
  bool allow_close_ = false;  // 저장 확인을 거쳐 닫기로 했다
  QProgressDialog* print_progress_ = nullptr;
  bool print_prefs_applied_ = false;  // 문서마다 처음 인쇄할 때 문서 인쇄 기본값을 대화상자에 넣는다
  QTimer* outline_highlight_timer_ = nullptr;
  QTimer* status_timer_ = nullptr;
  engine::TaskHandle outline_task_;  // 진행 중인 목차 로드 (문서 교체 시 취소)
  int outline_generation_ = 0;   // 목차 로드마다 증가 (문서 열기·구조 편집 뒤 재로드). 늦게 온 목차를 버린다
  int document_generation_ = 0;  // 문서를 열 때마다 증가. 늦게 온 저장·위치 변환 결과를 버린다
  std::unique_ptr<view::SearchSession> search_;
  int search_scroll_anchor_ = 0;  // 검색 시작 시 스크롤 위치. 사용자가 움직였으면 첫 결과로 끌고 가지 않는다
  QString screenshot_path_;
  // 선언 순서 중요: print_job_ 이 printer_ 를 참조하므로 printer_ 가 먼저 선언되어 나중에 소멸한다
  std::unique_ptr<QPrinter> printer_;
  std::unique_ptr<PrintJob> print_job_;
};

}  // namespace onpdf::app
