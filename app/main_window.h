#pragma once

#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/view/search.h"
#include "onpdf/core/view/text_index.h"
#include "onpdf/edit/font_catalog.h"
#include "onpdf/edit/font_face.h"
#include "onpdf/edit/form_commands.h"
#include "onpdf/edit/object_commands.h"

#include "document_session.h"
#include "document_view.h"
#include "edit_controller.h"
#include "font_catalog_loader.h"
#include "print_job.h"
#include "structure_job.h"
#include <QColor>
#include <QImage>
#include <QMainWindow>
#include <QPointer>
#include <QString>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

class QCloseEvent;
class QLabel;
class QMessageBox;
class QPrinter;
class QToolBar;
class QActionGroup;
class QComboBox;
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
  [[nodiscard]] QAction* delete_annotation_action() const noexcept {
    return delete_annotation_action_;
  }
  [[nodiscard]] QToolBar* annotation_toolbar() const noexcept { return annotation_toolbar_; }
  [[nodiscard]] engine::Color annotation_color() const noexcept { return annotation_color_; }
  [[nodiscard]] QToolButton* color_button() const noexcept { return color_button_; }
  [[nodiscard]] QToolButton* width_button() const noexcept { return width_button_; }
  [[nodiscard]] QToolButton* opacity_button() const noexcept { return opacity_button_; }
  [[nodiscard]] double shape_border_width() const noexcept {
    return shape_border_width_;
  }  // 새 도형·펜 두께
  [[nodiscard]] std::uint8_t shape_alpha() const noexcept {
    return shape_alpha_;
  }  // 새 도형·펜 불투명도
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
  void set_annotation_color(
      engine::Color color);  // 새 주석 색. 주석이 선택돼 있으면 그 주석의 색을 바꾼다
  // 선택한 도형·펜의 두께 / 선택한 주석의 불투명도를 Command 로 바꾼다. 선택이 없으면 새 도형·펜의
  // 기본값만 바꾼다
  void set_annotation_border_width(double width);
  void set_annotation_opacity(std::uint8_t alpha);
  bool add_markup_from_selection(engine::AnnotationKind kind);
  bool delete_selected_annotation();
  // 메모 생성: contents 가 주어지면 대화상자 없이 만든다 (테스트용)
  bool add_note(int page, onpdf::Point pt, const QString& contents);
  // ---- 텍스트 추가 (#45). 번들 폰트가 없으면 도구 비활성 ----
  // 텍스트 상자 생성: 대화상자 없이 (테스트용·on_text_requested 가 부른다). 시작하면 true
  bool add_text(int page, onpdf::Point pt, const QString& utf8);
  bool set_text_font(const QString& path);  // 번들 폰트 교체 (테스트: 저장소 서브셋 폰트)
  bool delete_selected_text_box();  // 선택한 쪽 객체(텍스트 상자·이미지) 삭제 (Delete 키·툴바와
                                    // 같은 경로). 시작하면 true
  // ---- 이미지 (#49) ----
  // 파일을 읽어 page 의 origin 에 놓는다 (center 면 가운데로). 대화상자 없이 (테스트·드롭·도구가
  // 부른다). 시작하면 true
  bool add_image_file(const QString& path, int page, onpdf::Point origin, bool center);
  bool add_image(const QImage& image, int page, onpdf::Point origin, bool center);
  // 클립보드의 이미지(또는 이미지 파일 URL)를 현재 쪽 가운데에 놓는다. 시작하면 true
  bool paste_image();
  // 폼 채우기 (#51). 선택지(콤보·목록)는 메뉴가 이 함수를 부른다 (테스트도)
  bool choose_form_option(int page, const engine::FieldRef& ref, std::vector<int> indices);
  // 열린 오버레이 편집기를 확정하고 폼 편집이 끝날 때까지 기다린다 (저장·닫기·인쇄 전)
  void settle_form_edits();
  [[nodiscard]] QAction* paste_action() const noexcept { return paste_action_; }
  [[nodiscard]] bool has_text_font() const noexcept { return text_font_ != nullptr; }
  // 시스템 글꼴 카탈로그 (#73). 로더가 끝나면 자동으로 들어오고, 테스트는 가짜 카탈로그를 주입한다
  void set_font_catalog(std::shared_ptr<const edit::FontCatalog> catalog);
  [[nodiscard]] bool has_font_catalog() const noexcept { return font_catalog_ != nullptr; }
  // 카탈로그가 없고 스캔도 안 했으면 스캔을 시작한다 (텍스트 도구 첫 사용·첫 대체 요청 때 — 시작
  // 때마다 글꼴 폴더를 훑지 않는다. Windows CI 에서 창마다 스캔하면 앱 테스트가 47s → 120s 초과).
  // 테스트는 카탈로그를 주입하므로 스캔하지 않는다
  void ensure_font_catalog();
  [[nodiscard]] double text_size() const noexcept { return text_size_; }
  void set_text_size(double size);
  [[nodiscard]] QComboBox* text_size_combo() const noexcept { return text_size_combo_; }
  // ---- 병합·분할 (대화상자 없이 같은 경로를 타는 진입점, 테스트용으로도 쓴다). 시작하면 true ----
  bool merge_files(std::vector<engine::StructureInput> inputs, const QString& output);
  bool split_document(int every,
                      const QString& directory);  // 현재 문서(디스크 파일)를 every 쪽마다
  void session_open_for_test(const QString& path, const QString& password) {
    session_.open(path, password);
  }  // 암호 대화상자 우회
  // 이 경로로 저장한다 (대화상자 없이 — 다른 이름으로 저장의 뒷부분, 테스트용). 시작하면 true
  bool save_to(const QString& path);
  // 대화상자 없이 이 프린터 설정으로 인쇄를 시작한다 (파일 > 인쇄 의 뒷부분, 테스트용). 시작하면
  // true
  bool print_with(QPrinter& printer);  // 프린터 설정으로 요청을 만들어 인쇄 (테스트용 진입점)
  bool print_with(QPrinter& printer, PrintJob::Request request);
  // 첫 쪽이 그려지면 창을 PNG 로 저장하고 앱을 종료한다 (QA·CI 스크린샷용, --screenshot 옵션).
  void screenshot_after_render(const QString& png_path);
  // 워커 멈춤 감시 (#121): 렌더 작업이 threshold 를 넘으면 "응답 없음" 경고. 테스트는 짧게 주입하고
  // 강제 종료(std::_Exit)를 훅으로 가로챈다
  void set_stall_threshold(std::chrono::milliseconds threshold) { stall_threshold_ = threshold; }
  void set_force_exit_hook(std::function<void(int code)> hook) { force_exit_ = std::move(hook); }
  [[nodiscard]] bool worker_stalled() const;

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
  void on_text_requested(int page, onpdf::Point pt);
  void on_text_box_selected(bool has, const DocumentView::SelectedTextBox& box);
  void on_image_requested(int page, onpdf::Point pt);
  void on_object_resize_requested(int page, const QString& id, onpdf::Rect before,
                                  onpdf::Rect after);
  void on_text_width_requested(int page, const QString& box, double left_x, double width);
  void on_object_nudge_requested(int page, const std::vector<std::string>& ids, onpdf::Point delta);
  void explain_locked_object();  // 기존 이미지 편집을 막은 이유 (#57)
  void on_form_field_clicked(int page, const onpdf::engine::FormWidgetInfo& widget);
  void on_form_text_committed(int page, const onpdf::engine::FormWidgetInfo& widget,
                              const QString& text);
  void show_form_choice_menu(int page, const onpdf::engine::FormWidgetInfo& widget);
  void run_form_command(std::shared_ptr<edit::Command> command);
  void flush_form_commands();
  void flush_pending_nudge();
  bool run_add_image(int page, engine::ImageSpec spec);
  void on_text_move_requested(int page, const std::vector<std::string>& ids, onpdf::Point delta);
  void on_text_delete_requested(int page, const QString& box);
  void on_text_edit_requested(int page, const DocumentView::SelectedTextBox& box);
  // 기존 텍스트 줄 (#65)
  void on_text_line_selected(bool has, int page, const engine::TextLineInfo& info);
  void on_text_line_edit_requested(int page, const engine::TextLineInfo& info);
  void on_text_line_delete_requested(int page, const engine::TextLineInfo& info);
  bool run_line_replace(int page, const std::string& line_id, std::optional<QString> text,
                        std::optional<double> size, std::optional<engine::Color> color,
                        std::shared_ptr<const edit::FontFace> fallback_face);
  void announce_line_edit();  // 줄 수정 Command 가 끝나면 새 상자를 고르고 대체 글꼴 안내
  [[nodiscard]] QString line_reason_text(const engine::TextLineInfo& info) const;
  bool replace_selected_text(
      std::optional<QString> utf8, std::optional<double> size, std::optional<engine::Color> color,
      std::shared_ptr<const edit::FontFace> face = nullptr);  // face 가 없으면 번들
  void load_bundled_text_font();
  // 대체 글꼴 (#73): 카탈로그에서 base_font_name 과 같은 이름 → 한글이면 기본 Sans → 번들. 후보마다
  // 글자가 다 있는지 본다. 서명 문서는 CFF 후보를 제외한다(증분 저장은 후처리를 못 한다)
  [[nodiscard]] std::shared_ptr<const edit::FontFace> fallback_font_for(
      const std::string& base_font_name, const QString& text);
  [[nodiscard]] std::shared_ptr<const edit::FontFace> load_catalog_font(
      const edit::FontEntry& entry);
  void on_ink_requested(int page, const std::vector<onpdf::Point>& points, onpdf::Rect bbox);
  void on_shape_requested(int page, onpdf::engine::AnnotationKind kind, onpdf::Rect rect);
  void update_property_buttons();
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
  // 인쇄 금지 문서의 권한(소유자) 암호 해제 (#23): 안내 → maybe_save → 암호 → 같은 파일을 다시 연다
  void offer_print_unlock();

 public:
  // 테스트용: 권한 암호 해제 뒤 인쇄 대화상자를 자동으로 열지 않는다 (macOS offscreen 의 네이티브
  // QPrintDialog 는 닫을 수 없다)
  void set_auto_print_after_unlock(bool on) noexcept { auto_print_after_unlock_ = on; }

 private:
  void prompt_print_unlock_password(const QString& note);
  void on_print_finished(PrintJob::Outcome outcome);
  void cancel_print();
  // 인쇄 중 새 문서를 열려 할 때 확인. "인쇄 취소하고 열기" 면 인쇄를 취소하고 true
  bool confirm_cancel_print_for(const QString& path);
  void
  discard_print_job();  // 진행 중이던 인쇄를 조용히 버리고 진행 창을 닫는다 (새 문서가 열렸을 때)
  void update_print_action();
  void on_outline_item(const onpdf::view::Link& target);
  // 쪽 대상으로 이동: location(링크)이면 바로, user_location(목차)이면 워커에서 변환 후, 둘 다
  // 없으면 쪽 맨 위
  void navigate_to(const onpdf::view::Link& target);
  void update_outline_highlight();
  void on_zoom_changed(double zoom);
  void refresh_status();
  void check_worker_stall();  // 1초마다: 렌더 작업이 오래 돌면 경고 (#121)
  void show_stall_warning();  // 창 모달 "응답 없음" 상자 (objectName worker_stalled)
  void on_page_refused(int page, const QString& reason);
  void on_tool_blocked(int page);
  void show_hazard_notice();  // 열 때 위험 쪽 목록 안내 (objectName render_hazards_notice)
  void set_document_status(const QString& text);  // 문서 정보 (지속)
  void show_transient(const QString& text);       // 진행·오류 안내 (몇 초 뒤 사라짐)
  void fail_screenshot(const QString& reason);    // --screenshot 모드에서 실패 시 종료

  UiDispatcher& dispatcher_;
  engine::PdfiumWorker& worker_;  // 멈춤 감시용 (#121). main.cpp 의 스택 객체 — 창보다 오래 산다
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
  std::vector<std::filesystem::path>
      structure_outputs_;  // 진행 중 작업의 출력 (취소·실패 시 정리, 성공 시 열기)
  enum class StructureKind { kNone, kMerge, kSplit } structure_kind_ = StructureKind::kNone;
  QString structure_folder_;
  std::vector<engine::StructureInput> pending_merge_inputs_;  // 암호를 물어 다시 시도할 병합
  QString pending_merge_output_;
  int merge_password_attempts_ = 0;
  QToolBar* annotation_toolbar_ = nullptr;
  QActionGroup* tool_group_ = nullptr;
  QAction* tool_actions_[kAnnotationToolCount] = {};  // AnnotationTool 순서
  QToolButton* color_button_ = nullptr;
  QToolButton* width_button_ = nullptr;  // 두께 ▾ (도형·펜)
  QMenu* width_menu_ = nullptr;
  QMenu* opacity_menu_ = nullptr;
  bool selection_status_shown_ =
      false;  // 상태 표시줄이 선택 정보를 보이는 중 — 해제 때 그것만 지운다 (안내 문구는 남긴다)
  QToolButton* opacity_button_ = nullptr;  // 불투명도 ▾
  double shape_border_width_ = 2.0;
  std::uint8_t shape_alpha_ = 255;
  std::shared_ptr<const edit::FontFace> text_font_;  // 새 텍스트 글꼴 (불변, Command 와 공유)
  FontCatalogLoader* catalog_loader_ = nullptr;      // 시스템 글꼴 스캔 (#73)
  std::shared_ptr<const edit::FontCatalog> font_catalog_;
  bool font_scan_started_ = false;
  std::vector<std::pair<std::string, std::shared_ptr<const edit::FontFace>>>
      fallback_cache_;                              // "경로#면" → 글꼴, 최근 8개
  std::weak_ptr<edit::Command> pending_line_edit_;  // 실행 중인 줄 수정 Command (#65)
  QString pending_line_notice_;                     // 새 상자가 선택된 뒤 보여 줄 안내 (대체 글꼴)
  QString text_font_path_;
  QComboBox* text_size_combo_ = nullptr;
  double text_size_ = 24.0;
  bool syncing_text_size_ =
      false;  // 선택한 상자의 크기를 콤보에 비출 때 기본값·Command 를 건드리지 않는다
  QAction* delete_annotation_action_ = nullptr;
  QAction* markup_selection_actions_[3] = {};  // 하이라이트·밑줄·취소선 (선택 영역)
  engine::Color annotation_color_{255, 255, 0, 255};
  bool annotation_edits_allowed_ =
      false;  // idle && 권한. 도구 선택은 권한만 보고, 실제 편집 요청은 이것으로 막는다
  bool text_edits_allowed_ = false;  // idle && 내용 수정 권한(비트 4) && 번들 글꼴 (리뷰 W2)
  bool content_edits_allowed_ =
      false;  // idle && 내용 수정 권한(비트 4) — 이미지·객체 이동·크기·삭제 (#49, 글꼴 불필요)
  // 마지막 이미지 추가: 완료되면 새 이미지를 고르고, PDFium 이 JPEG 를 못 넣으면 픽셀로 다시
  // 시도한다
  struct PendingImage {
    QString path;  // 비면 메모리 이미지 (재시도 없음)
    int page = 0;
    onpdf::Point origin;
    bool center = false;
    bool jpeg = false;
  };
  std::optional<PendingImage> pending_image_;
  std::shared_ptr<edit::AddImageCommand> last_image_command_;
  // 화살표 연타: 편집 중(busy)이면 모았다가 끝나면 한 번에 보낸다 (스택에서는 키보드 이동끼리
  // 합쳐진다)
  struct PendingNudge {
    int page = 0;
    std::vector<std::string> ids;
    onpdf::Point delta;
  };
  std::optional<PendingNudge> pending_nudge_;
  // 폼 Command 대기열 (#51): Tab 으로 연달아 입력하면 앞 편집이 끝나기 전에 다음 확정이 온다
  std::deque<std::shared_ptr<edit::Command>> pending_form_commands_;
  bool form_edits_allowed_ = false;
  bool form_in_flight_ = false;  // 대기열에서 보낸 폼 Command 가 워커에서 도는 중 (settle 은 이것만
                                 // 기다린다)  // 권한(fill_forms)만. 바쁜 동안은 대기열에 쌓는다
  QAction* paste_action_ = nullptr;
  engine::Color text_color_{0, 0, 0,
                            255};  // 새 텍스트 색 (기본 검정; 색 팔레트가 주석 색과 함께 바꾼다)
  bool updating_actions_ = false;  // update_edit_actions 재진입 차단
  struct PendingInsert {
    QString path;
    int at = 0;
  };
  std::optional<PendingInsert>
      pending_insert_;             // 암호가 필요하면 물은 뒤 같은 삽입을 다시 시도한다
  bool signature_warned_ = false;  // 서명 문서 구조 편집 경고는 문서마다 한 번
  EditController* edit_ = nullptr;
  bool saving_ = false;
  bool allow_close_ = false;  // 저장 확인을 거쳐 닫기로 했다
  QProgressDialog* print_progress_ = nullptr;
  struct PendingPrintUnlock {
    int page = 0;  // 다시 연 뒤 되살릴 현재 쪽
  };
  std::optional<PendingPrintUnlock> print_unlock_;  // 권한 암호로 다시 여는 중 (#23)
  bool auto_print_after_unlock_ = true;
  bool print_prefs_applied_ =
      false;  // 문서마다 처음 인쇄할 때 문서 인쇄 기본값을 대화상자에 넣는다
  QTimer* outline_highlight_timer_ = nullptr;
  QTimer* status_timer_ = nullptr;
  // 워커 멈춤 감시 (#121)
  QTimer* stall_timer_ = nullptr;
  std::chrono::milliseconds stall_threshold_{60000};
  std::function<void(int code)> force_exit_;
  QPointer<QMessageBox> stall_box_;
  std::optional<std::chrono::steady_clock::time_point> warned_since_;  // 경고한 작업의 시작 시각
  engine::TaskHandle outline_task_;  // 진행 중인 목차 로드 (문서 교체 시 취소)
  int outline_generation_ =
      0;  // 목차 로드마다 증가 (문서 열기·구조 편집 뒤 재로드). 늦게 온 목차를 버린다
  int document_generation_ = 0;  // 문서를 열 때마다 증가. 늦게 온 저장·위치 변환 결과를 버린다
  std::unique_ptr<view::SearchSession> search_;
  int search_scroll_anchor_ =
      0;  // 검색 시작 시 스크롤 위치. 사용자가 움직였으면 첫 결과로 끌고 가지 않는다
  QString screenshot_path_;
  // 선언 순서 중요: print_job_ 이 printer_ 를 참조하므로 printer_ 가 먼저 선언되어 나중에 소멸한다
  std::unique_ptr<QPrinter> printer_;
  std::unique_ptr<PrintJob> print_job_;
};

}  // namespace onpdf::app
