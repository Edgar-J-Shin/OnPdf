#include "main_window.h"

#include "onpdf/core/engine/qpdf_tools.h"
#include "onpdf/core/engine/save.h"
#include "onpdf/core/page_ranges.h"
#include "onpdf/core/platform/font_dirs.h"
#include "onpdf/core/platform/path.h"
#include "onpdf/edit/add_text_command.h"
#include "onpdf/edit/annotation_commands.h"
#include "onpdf/edit/delete_pages.h"
#include "onpdf/edit/insert_pages.h"
#include "onpdf/edit/move_pages.h"
#include "onpdf/edit/object_commands.h"
#include "onpdf/edit/page_ops.h"
#include "onpdf/edit/rotate_page.h"
#include "onpdf/edit/text_box_commands.h"
#include "onpdf/edit/text_line_commands.h"

#include "annotation_dialog.h"
#include "crash_handler.h"
#include "crash_report.h"
#include "document_view.h"
#include "form_field_editor.h"
#include "image_import.h"
#include "outline_panel.h"
#include "palette.h"
#include "print_preview_dialog.h"
#include "search_bar.h"
#include "settings_dialog.h"
#include "thumbnail_panel.h"
#include <QAbstractButton>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPrintDialog>
#include <QPrinter>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

namespace onpdf::app {

MainWindow::MainWindow(engine::PdfiumWorker& worker, UiDispatcher& dispatcher, QWidget* parent)
    : QMainWindow(parent),
      settings_(std::make_unique<Settings>()),
      dispatcher_(dispatcher),
      worker_(worker),
      session_(worker, dispatcher, this),
      force_exit_([](int code) { std::_Exit(code); }) {
  setWindowTitle(QStringLiteral("OnPdf"));
  setAcceptDrops(true);

  auto* central = new QWidget(this);
  auto* column = new QVBoxLayout(central);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(0);
  search_bar_ = new SearchBar(central);
  view_ = new DocumentView(central);
  view_->clear_document(tr("Open a file or drop it here (Ctrl+O)"));
  column->addWidget(search_bar_);
  column->addWidget(view_, 1);

  // 사이드바(썸네일·목차) | 문서
  sidebar_ = new QTabWidget(this);
  sidebar_->setDocumentMode(true);
  thumbnail_panel_ = new ThumbnailPanel(sidebar_);
  outline_panel_ = new OutlinePanel(sidebar_);
  sidebar_->addTab(thumbnail_panel_, tr("Thumbnails"));
  sidebar_->addTab(outline_panel_, tr("Contents"));
  sidebar_->setMinimumWidth(140);
  sidebar_->setMaximumWidth(400);
  splitter_ = new QSplitter(Qt::Horizontal, this);
  splitter_->addWidget(sidebar_);
  splitter_->addWidget(central);
  splitter_->setChildrenCollapsible(false);
  splitter_->setStretchFactor(0, 0);
  splitter_->setStretchFactor(1, 1);
  splitter_->setSizes({200, 700});
  setCentralWidget(splitter_);

  outline_highlight_timer_ = new QTimer(this);
  outline_highlight_timer_->setObjectName(QStringLiteral("outline_highlight_timer"));
  outline_highlight_timer_->setSingleShot(true);
  outline_highlight_timer_->setInterval(100);
  connect(outline_highlight_timer_, &QTimer::timeout, this, &MainWindow::update_outline_highlight);

  page_spin_ = new QSpinBox(this);
  page_spin_->setRange(1, 1);
  page_spin_->setEnabled(false);
  page_spin_->setKeyboardTracking(false);
  page_total_label_ = new QLabel(this);
  status_label_ = new QLabel(this);
  link_label_ = new QLabel(this);
  statusBar()->addWidget(page_spin_);
  statusBar()->addWidget(page_total_label_);
  statusBar()->addWidget(status_label_, 1);
  statusBar()->addPermanentWidget(link_label_);
  set_document_status(tr("No document"));

  edit_ = new EditController(dispatcher_, this);
  structure_job_ = new StructureJob(dispatcher_, this);
  catalog_loader_ = new FontCatalogLoader(dispatcher_, this);
  connect(catalog_loader_, &FontCatalogLoader::ready, this, [this] {
    if (font_catalog_ == nullptr)
      set_font_catalog(catalog_loader_->catalog());  // 테스트가 먼저 주입했으면 그대로
  });
  connect(structure_job_, &StructureJob::finished, this, &MainWindow::on_structure_finished);
  build_menus();
  build_annotation_ui();
  connect(edit_, &EditController::document_changed, this, &MainWindow::on_document_changed);
  connect(view_, &DocumentView::markup_requested, this, &MainWindow::on_markup_requested);
  connect(view_, &DocumentView::note_requested, this, &MainWindow::on_note_requested);
  connect(view_, &DocumentView::text_requested, this, &MainWindow::on_text_requested);
  connect(view_, &DocumentView::text_box_selected, this, &MainWindow::on_text_box_selected);
  connect(view_, &DocumentView::text_box_reselected, this,
          [this](const DocumentView::SelectedTextBox&) {
            update_edit_actions();
            update_property_buttons();
          });
  connect(view_, &DocumentView::text_move_requested, this, &MainWindow::on_text_move_requested);
  connect(view_, &DocumentView::text_delete_requested, this, &MainWindow::on_text_delete_requested);
  connect(view_, &DocumentView::text_edit_requested, this, &MainWindow::on_text_edit_requested);
  connect(view_, &DocumentView::image_requested, this, &MainWindow::on_image_requested);
  connect(view_, &DocumentView::object_resize_requested, this,
          &MainWindow::on_object_resize_requested);
  connect(view_, &DocumentView::text_width_requested, this, &MainWindow::on_text_width_requested);
  connect(view_, &DocumentView::object_nudge_requested, this,
          &MainWindow::on_object_nudge_requested);
  connect(view_, &DocumentView::locked_object_edit_attempted, this,
          &MainWindow::explain_locked_object);
  connect(view_, &DocumentView::text_line_selected, this, &MainWindow::on_text_line_selected);
  connect(view_, &DocumentView::text_line_edit_requested, this,
          &MainWindow::on_text_line_edit_requested);
  connect(view_, &DocumentView::text_line_delete_requested, this,
          &MainWindow::on_text_line_delete_requested);
  connect(view_, &DocumentView::form_field_clicked, this, &MainWindow::on_form_field_clicked);
  connect(view_, &DocumentView::form_text_committed, this, &MainWindow::on_form_text_committed);
  connect(view_, &DocumentView::ink_requested, this, &MainWindow::on_ink_requested);
  connect(view_, &DocumentView::shape_requested, this, &MainWindow::on_shape_requested);
  connect(view_, &DocumentView::annotation_selected, this, &MainWindow::on_annotation_selected);
  // 편집 뒤 자동 재선택: 버튼·메뉴만 맞추고 상태 표시줄 안내("다시 만들었습니다" 등)는 그대로 둔다
  connect(view_, &DocumentView::annotation_reselected, this, [this](const engine::AnnotationInfo&) {
    update_edit_actions();
    update_property_buttons();
  });
  connect(view_, &DocumentView::annotation_edit_requested, this,
          &MainWindow::on_annotation_edit_requested);
  connect(view_, &DocumentView::annotation_delete_requested, this,
          [this](int, const QString&) { delete_selected_annotation(); });
  connect(edit_, &EditController::state_changed, this, [this] {
    if (edit_->busy()) set_action(QStringLiteral("edit"));  // (#131)
    update_edit_actions();
    update_title();
    announce_line_edit();
    flush_pending_nudge();
    if (!edit_->busy()) form_in_flight_ = false;
    flush_form_commands();
  });
  connect(edit_, &EditController::failed, this, &MainWindow::on_edit_failed);

  connect(&session_, &DocumentSession::opened, this, &MainWindow::on_opened);
  connect(&session_, &DocumentSession::password_required, this, &MainWindow::on_password_required);
  connect(&session_, &DocumentSession::open_failed, this, &MainWindow::on_open_failed);
  connect(view_, &DocumentView::current_page_changed, this, &MainWindow::on_current_page_changed);
  connect(view_, &DocumentView::current_page_changed, this,
          [this](int page) { set_action(QStringLiteral("render p.%1").arg(page + 1)); });  // (#131)
  connect(view_, &DocumentView::zoom_changed, this, &MainWindow::on_zoom_changed);
  connect(view_, &DocumentView::first_view_rendered, this, &MainWindow::on_first_view_rendered);
  // 타일이 도착할 때마다 캐시·렌더 통계를 갱신하되 1초에 한 번으로 묶는다
  status_timer_ = new QTimer(this);
  status_timer_->setObjectName(QStringLiteral("status_timer"));
  status_timer_->setSingleShot(true);
  status_timer_->setInterval(1000);
  connect(status_timer_, &QTimer::timeout, this, &MainWindow::refresh_status);
  connect(view_, &DocumentView::tiles_changed, this, [this] {
    if (!status_timer_->isActive()) status_timer_->start();
  });
  // 워커 멈춤 감시 (#121): 렌더 작업이 상한을 넘으면 "응답 없음" 경고
  stall_timer_ = new QTimer(this);
  stall_timer_->setObjectName(QStringLiteral("stall_timer"));
  stall_timer_->setInterval(1000);
  connect(stall_timer_, &QTimer::timeout, this, &MainWindow::check_worker_stall);
  stall_timer_->start();
  connect(view_, &DocumentView::page_refused, this, &MainWindow::on_page_refused);
  connect(view_, &DocumentView::tool_blocked, this, &MainWindow::on_tool_blocked);
  connect(view_, &DocumentView::selection_changed, this, [this](bool has) {
    copy_action_->setEnabled(has);
    update_edit_actions();
  });
  connect(view_, &DocumentView::link_activated, this, &MainWindow::on_link_activated);
  connect(view_, &DocumentView::link_hovered, this, &MainWindow::on_link_hovered);
  connect(search_bar_, &SearchBar::query_changed, this, &MainWindow::on_search_query);
  connect(search_bar_, &SearchBar::next_requested, this, &MainWindow::on_find_next);
  connect(search_bar_, &SearchBar::previous_requested, this, &MainWindow::on_find_previous);
  connect(search_bar_, &SearchBar::closed, this, &MainWindow::on_search_closed);
  connect(thumbnail_panel_, &ThumbnailPanel::page_activated, this, [this](int page) {
    if (view_->has_document()) view_->go_to_page(page);
  });
  connect(thumbnail_panel_, &ThumbnailPanel::move_requested, this,
          [this](const std::vector<int>& pages, int drop_row) { move_pages(pages, drop_row); });
  connect(thumbnail_panel_, &ThumbnailPanel::insert_file_requested, this,
          [this](const QString& path, int drop_row) { insert_pages_from(path, drop_row); });
  connect(thumbnail_panel_, &ThumbnailPanel::delete_requested, this,
          [this](const std::vector<int>& pages) { delete_pages(pages); });
  connect(thumbnail_panel_, &ThumbnailPanel::context_menu_requested, this,
          &MainWindow::on_thumbnail_context_menu);
  connect(outline_panel_, &OutlinePanel::item_activated, this, &MainWindow::on_outline_item);
  connect(view_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
    // 스로틀: 진행 중이면 다시 시작하지 않는다 → 연속(관성) 스크롤 중에도 100ms 마다 갱신
    if (sidebar_->isVisible() && sidebar_->currentWidget() == outline_panel_ &&
        !outline_highlight_timer_->isActive()) {
      outline_highlight_timer_->start();
    }
  });
  connect(sidebar_, &QTabWidget::currentChanged, this, [this] { update_outline_highlight(); });
  connect(page_spin_, &QSpinBox::valueChanged, this, [this](int value) {
    if (view_->has_document() && view_->current_page() != value - 1) view_->go_to_page(value - 1);
  });

  // 설정 (#129): 편집 기본값·캐시·멈춤 초를 반영하고 창 상태를 복원한다. 그 뒤의 변경은 changed 로
  apply_settings();
  restore_window_state();
  connect(settings_.get(), &Settings::changed, this, &MainWindow::apply_settings);
  load_bundled_text_font();  // 모든 위젯·액션이 만들어진 뒤 (update_edit_actions 를 부른다)
}

MainWindow::~MainWindow() {
  print_job_.reset();          // 신호 없이 취소 (printer_ 보다 먼저)
  view_->set_search(nullptr);  // 세션이 먼저 사라지므로 뷰가 댕글 포인터를 들지 않게
}

void MainWindow::build_menus() {
  QMenu* file = menuBar()->addMenu(tr("&File"));
  QAction* open =
      file->addAction(tr("&Open..."), QKeySequence::Open, this, &MainWindow::on_open_action);
  recent_menu_ = file->addMenu(tr("Open &Recent"));  // (#129)
  recent_menu_->setObjectName(QStringLiteral("recent_menu"));
  connect(recent_menu_, &QMenu::aboutToShow, this, &MainWindow::rebuild_recent_menu);
  rebuild_recent_menu();
  open->setStatusTip(tr("Open a PDF file"));
  save_action_ = file->addAction(tr("&Save"), QKeySequence::Save, this, &MainWindow::on_save);
  save_as_action_ =
      file->addAction(tr("Save &As..."), QKeySequence::SaveAs, this, &MainWindow::on_save_as);
  file->addSeparator();
  merge_action_ = file->addAction(tr("&Merge..."), this, &MainWindow::on_merge_action);
  merge_action_->setStatusTip(
      tr("Combine several PDFs into one file (keeps the first document's outline and forms)"));
  split_action_ = file->addAction(tr("Spli&t..."), this, &MainWindow::on_split_action);
  split_action_->setStatusTip(tr("Split the current document every N pages"));
  split_action_->setEnabled(false);
  file->addSeparator();
  print_action_ =
      file->addAction(tr("&Print..."), QKeySequence::Print, this, &MainWindow::on_print);
  print_action_->setEnabled(false);
  file->addSeparator();
  // QKeySequence::Quit 은 Windows 에서 비어 있으므로 Ctrl+Q 를 명시한다
  // 종료는 창 닫기로: closeEvent 에서 저장 확인을 거친다 (마지막 창이 닫히면 앱이 끝난다)
  QAction* quit =
      file->addAction(tr("&Quit"), QKeySequence(Qt::CTRL | Qt::Key_Q), this, &QWidget::close);
  quit->setMenuRole(QAction::QuitRole);

  QMenu* edit = menuBar()->addMenu(tr("&Edit"));
  undo_action_ = edit->addAction(tr("&Undo"), QKeySequence::Undo, this, [this] {
    // 처음 선택 없던 라디오 그룹의 첫 선택: PDFium 이 해제를 지원하지 않아 문서는 그대로다 (#51)
    const auto target = edit_->undo_command();
    const auto* radio = dynamic_cast<const edit::SetFormCheckedCommand*>(target.get());
    if (edit_->undo() && radio != nullptr && !radio->undoable()) {
      show_transient(tr("Radio buttons cannot be deselected, so the first choice stays"));
    }
  });
  redo_action_ = edit->addAction(tr("&Redo"), QKeySequence::Redo, this, [this] { edit_->redo(); });
  edit->addSeparator();
  paste_action_ =
      edit->addAction(tr("Paste &Image"), QKeySequence::Paste, this, [this] { paste_image(); });
  paste_action_->setObjectName(QStringLiteral("paste_image"));
  paste_action_->setStatusTip(tr("Place the clipboard image at the center of the current page"));
  edit->addSeparator();
  copy_action_ = edit->addAction(tr("&Copy"), QKeySequence::Copy, this, &MainWindow::on_copy);
  copy_action_->setEnabled(false);
  select_all_action_ = edit->addAction(tr("Select &All on Page"), QKeySequence::SelectAll, this,
                                       &MainWindow::on_select_all);
  edit->addSeparator();
  edit->addAction(tr("&Find..."), QKeySequence::Find, this, &MainWindow::open_find);
  find_next_action_ =
      edit->addAction(tr("Find &Next"), QKeySequence::FindNext, this, &MainWindow::on_find_next);
  find_previous_action_ = edit->addAction(tr("Find &Previous"), QKeySequence::FindPrevious, this,
                                          &MainWindow::on_find_previous);
  edit->addSeparator();
  preferences_action_ = edit->addAction(tr("&Preferences..."), QKeySequence::Preferences, this,
                                        &MainWindow::open_preferences);  // (#129)
  preferences_action_->setObjectName(QStringLiteral("preferences"));
  preferences_action_->setMenuRole(QAction::PreferencesRole);  // macOS 는 앱 메뉴 ⌘,
  if (preferences_action_->shortcut().isEmpty())  // Windows·Linux 에는 표준 키가 없다 → Ctrl+,
    preferences_action_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));

  QMenu* page = menuBar()->addMenu(tr("&Page"));
  rotate_cw_action_ = page->addAction(tr("Rotate &Right"), QKeySequence(Qt::CTRL | Qt::Key_R), this,
                                      [this] { rotate_current_page(1); });
  rotate_ccw_action_ =
      page->addAction(tr("Rotate &Left"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R), this,
                      [this] { rotate_current_page(3); });
  page->addSeparator();
  // Delete 단축키는 썸네일 목록이 직접 처리한다 (목록 밖에서는 현재 쪽). 메뉴에 단축키를 두면
  // 목록의 키와 겹친다
  delete_pages_action_ =
      page->addAction(tr("&Delete Pages"), this, [this] { delete_pages(target_pages()); });
  move_up_action_ = page->addAction(tr("Move &Up"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Up),
                                    this, [this] { move_selection(-1); });
  move_down_action_ =
      page->addAction(tr("Move Do&wn"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Down), this,
                      [this] { move_selection(+1); });
  page->addSeparator();
  insert_pages_action_ =
      page->addAction(tr("&Insert Pages from File..."), this, &MainWindow::on_insert_pages_action);

  QMenu* view = menuBar()->addMenu(tr("&View"));
  view->addAction(tr("Fit &Width"), QKeySequence(Qt::CTRL | Qt::Key_1), this,
                  [this] { view_->set_zoom_mode(ZoomMode::kFitWidth); });
  view->addAction(tr("Fit &Page"), QKeySequence(Qt::CTRL | Qt::Key_2), this,
                  [this] { view_->set_zoom_mode(ZoomMode::kFitPage); });
  view->addAction(tr("&Actual Size"), QKeySequence(Qt::CTRL | Qt::Key_0), this,
                  [this] { view_->set_zoom(1.0); });
  view->addSeparator();
  view->addAction(tr("Zoom &In"), QKeySequence::ZoomIn, this, [this] { view_->zoom_in(); });
  view->addAction(tr("Zoom &Out"), QKeySequence::ZoomOut, this, [this] { view_->zoom_out(); });
  view->addSeparator();
  sidebar_action_ =
      view->addAction(tr("&Sidebar"), QKeySequence(Qt::Key_F4), this, [this](bool checked) {
        sidebar_->setVisible(checked);
        settings_->set_sidebar_visible(checked);  // (#129)
        if (checked) update_outline_highlight();
      });
  sidebar_action_->setCheckable(true);
  sidebar_action_->setChecked(true);

  QMenu* help = menuBar()->addMenu(tr("&Help"));
  QAction* report_folder = help->addAction(tr("Open &Report Folder"), this,
                                           &MainWindow::open_report_folder);  // (#131)
  report_folder->setObjectName(QStringLiteral("open_report_folder"));
  help->addAction(tr("&About OnPdf"), this, [this] {
    QMessageBox::about(this, tr("About OnPdf"),
                       tr("OnPdf %1 — PDF viewer and editor based on PDFium\nApache License 2.0\n\n"
                          "PDFium %2 · Qt %3")
                           .arg(QApplication::applicationVersion(), pdfium_version(),
                                QString::fromLatin1(qVersion())));
  });
}

void MainWindow::screenshot_after_render(const QString& png_path) {
  screenshot_path_ = png_path;
  // 어떤 경로로든 렌더가 끝나지 않으면 CI 가 멈추지 않도록 워치독을 둔다
  QTimer::singleShot(15000, this, [this] {
    if (!screenshot_path_.isEmpty()) fail_screenshot(QStringLiteral("timeout"));
  });
}

void MainWindow::fail_screenshot(const QString& reason) {
  qWarning("screenshot failed: %s", qPrintable(reason));
  screenshot_path_.clear();
  QApplication::exit(1);
}

void MainWindow::open_file(const QString& path) {
  if (worker_stalled()) {  // 워커가 멈춰 있으면 저장도 열기도 못 한다 (#121)
    show_stall_warning();
    return;
  }
  // 수정된 문서면 먼저 저장 여부를 묻고, 그다음 인쇄 확인 (순서: 수정 → 인쇄)
  maybe_save([this, path] { continue_open(path); });
}

void MainWindow::continue_open(const QString& path) {
  // 인쇄 중이면 먼저 묻는다: 그대로 두면 열지 않고, 취소하고 열기를 고르면 인쇄를 취소한 뒤 연다
  if (print_job_ && print_job_->active() && !confirm_cancel_print_for(path)) {
    show_transient(tr("Not opened because a print job is running"));
    return;
  }
  show_transient(tr("Opening: %1").arg(QFileInfo(path).fileName()));
  print_unlock_.reset();
  // 세션 표식 (#131): 여는 중에 죽으면 다음 실행이 어떤 파일이었는지 안다
  SessionMarker marker;
  marker.path = QFileInfo(path).absoluteFilePath();
  marker.stage = QStringLiteral("opening");
  marker.since = QDateTime::currentDateTime();
  marker.last_action = QStringLiteral("open");
  marker.version = app_version();
  settings_->set_session_marker(marker);
  last_action_ = marker.last_action;
  crash::set_last_action("open");
  session_.open(path);
}

void MainWindow::on_open_action() {
  const QString path = QFileDialog::getOpenFileName(this, tr("Open PDF"), start_dir(DirKind::kOpen),
                                                    tr("PDF documents (*.pdf);;All files (*)"));
  if (!path.isEmpty()) open_file(path);
}

void MainWindow::on_opened(const QString& path, int page_count) {
  edit_->set_document(page_count > 0 ? session_.handle() : engine::DocumentHandle{});
  update_title();
  statusBar()->clearMessage();  // "여는 중" 임시 메시지가 문서 정보를 가리지 않게
  pending_form_commands_.clear();
  form_in_flight_ = false;
  if (page_count > 0 && session_.handle().info().form_type == engine::FormType::kXfaFull) {
    show_transient(tr("XFA forms are not supported (only the AcroForm part is shown)"));
  }
  if (page_count <= 0) {
    print_unlock_.reset();
    settings_->clear_session_marker();  // (#131)
    if (!screenshot_path_.isEmpty()) {
      fail_screenshot(QStringLiteral("no pages"));
      return;
    }
    reset_search();
    discard_print_job();
    print_prefs_applied_ = false;
    update_print_action();
    outline_task_.cancel();
    ++outline_generation_;
    ++document_generation_;
    pending_insert_.reset();
    thumbnail_panel_->clear_document();
    outline_panel_->set_outline(nullptr);
    view_->clear_document(tr("The document has no pages"));
    page_spin_->setEnabled(false);
    page_total_label_->clear();
    set_document_status(tr("%1 · 0 pages").arg(QFileInfo(path).fileName()));
    return;
  }
  page_spin_->setRange(1, page_count);
  page_spin_->setEnabled(true);
  page_total_label_->setText(tr("/ %1").arg(page_count));
  const StartZoom start_zoom = settings_->start_zoom();  // set_document 전에 읽는다 (#129)
  const ZoomKind last_kind = settings_->last_zoom_kind();
  const double last_zoom = settings_->last_zoom();
  view_->set_document(session_.handle(), dispatcher_);
  apply_start_zoom(start_zoom, last_kind, last_zoom);
  view_->setFocus();
  settings_->add_recent(path);
  remember_dir(DirKind::kOpen, path);
  settings_->set_session_stage(QStringLiteral("viewing"));  // (#131)
  set_action(QStringLiteral("view"));
  discard_print_job();
  print_prefs_applied_ = false;
  update_print_action();
  thumbnail_panel_->set_document(session_.handle(), dispatcher_);
  thumbnail_panel_->set_current_page(std::max(0, view_->current_page()));
  ++document_generation_;
  pending_insert_.reset();
  signature_warned_ = false;
  load_outline(true);
  reset_search();
  // 검색 바가 열려 있으면 새 문서에서 같은 검색어로 다시 찾는다
  if (search_bar_->isVisible() && !search_bar_->query().trimmed().isEmpty()) {
    on_search_query(search_bar_->query(), search_bar_->options());
  }
  refresh_status();
  show_hazard_notice();  // 렌더 위험 쪽이 있으면 한 번 알린다 (#121)
  if (print_unlock_) {   // 권한 암호로 다시 열었다 (#23)
    const PendingPrintUnlock unlock = *print_unlock_;
    print_unlock_.reset();
    if (unlock.page > 0) view_->go_to_page(unlock.page);
    if (session_.handle().info().print.allowed) {
      show_transient(tr("Printing unlocked"));
      if (auto_print_after_unlock_)
        QTimer::singleShot(0, this, &MainWindow::on_print);  // 바로 인쇄 대화상자로
    } else {
      prompt_print_unlock_password(tr("That is not the permissions password."));
    }
  }
}

// ---------------------------------------------------------------- 편집·저장

// 주석 속성 표시용 (두께는 도형·펜만)
namespace {
using engine::has_border_width;
QString width_text(double width) {
  return MainWindow::tr("%1 pt").arg(width);
}
QString opacity_text(double opacity) {
  return MainWindow::tr("%1%").arg(qRound(opacity * 100.0));
}
}  // namespace

QString MainWindow::command_label(std::string_view id) const {
  if (id == "rotate_page") return tr("Rotate Pages");
  if (id == "delete_pages") return tr("Delete Pages");
  if (id == "move_pages") return tr("Move Pages");
  if (id == "insert_pages") return tr("Insert Pages");
  if (id == "add_highlight") return tr("Add Highlight");
  if (id == "add_underline") return tr("Add Underline");
  if (id == "add_strikeout") return tr("Add Strikeout");
  if (id == "add_note") return tr("Add Note");
  if (id == "add_ink") return tr("Add Pen Stroke");
  if (id == "add_square") return tr("Add Rectangle");
  if (id == "add_circle") return tr("Add Ellipse");
  if (id == "add_text") return tr("Add Text");
  if (id == "add_image") return tr("Add Image");
  if (id == "resize_object") return tr("Resize");
  if (id == "remove_object") return tr("Delete");
  if (id == "move_object") return tr("Move Objects");
  if (id == "remove_text") return tr("Delete Text");
  if (id == "replace_text") return tr("Edit Text");
  if (id == "resize_text") return tr("Change Text Width");
  if (id == "replace_line") return tr("Edit Line");
  if (id == "format_line") return tr("Format Line");
  if (id == "remove_line") return tr("Delete Line");
  if (id == "form_text") return tr("Fill Form Field");
  if (id == "form_check" || id == "form_choice") return tr("Choose Form Option");
  if (id == "set_annotation_border_width") return tr("Change Line Width");
  if (id == "set_annotation_opacity") return tr("Change Opacity");
  if (id == "remove_annotation") return tr("Delete Annotation");
  if (id == "set_annotation_color") return tr("Change Annotation Color");
  if (id == "set_annotation_contents") return tr("Edit Note");
  return QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()));
}

void MainWindow::update_title() {
  if (!session_.has_document()) {
    setWindowTitle(QStringLiteral("OnPdf"));
    setWindowModified(false);
    return;
  }
  // [*] 자리에 수정 표시가 붙는다 (macOS 는 닫기 버튼의 점)
  setWindowTitle(QStringLiteral("%1[*] — OnPdf").arg(QFileInfo(session_.path()).fileName()));
  setWindowModified(edit_->is_modified());
}

void MainWindow::update_edit_actions() {
  if (updating_actions_) return;  // set_tool → annotation_selected → 여기로 되돌아오는 재진입 차단
  const QScopedValueRollback guard(updating_actions_, true);
  const bool has_pages = session_.has_document() && session_.handle().valid() &&
                         session_.handle().info().page_count > 0;
  const bool printing = print_job_ && print_job_->active();
  // 편집 가능 상태(작업 중이 아님)와 편집 종류별 문서 권한을 따로 본다
  const bool idle = has_pages && !edit_->busy() && !saving_ && !printing;
  const bool can_assemble =
      has_pages && session_.handle().info().permissions.assemble;  // 쪽 회전·삭제·삽입
  form_edits_allowed_ =
      has_pages && session_.handle().info().permissions.fill_forms;  // 폼 (#51): 바쁘면 대기열
  view_->set_form_fillable(form_edits_allowed_);
  rotate_cw_action_->setEnabled(idle && can_assemble);
  rotate_ccw_action_->setEnabled(idle && can_assemble);
  const QString assemble_tip =
      has_pages && !can_assemble
          ? tr("This document does not allow page editing (rotate, delete, insert)")
          : QString();
  rotate_cw_action_->setStatusTip(assemble_tip);
  rotate_ccw_action_->setStatusTip(assemble_tip);
  for (QAction* action :
       {delete_pages_action_, move_up_action_, move_down_action_, insert_pages_action_}) {
    action->setEnabled(idle && can_assemble);
    action->setStatusTip(assemble_tip);
  }
  thumbnail_panel_->set_editable(idle && can_assemble);
  const bool can_annotate = has_pages && session_.handle().info().permissions.annotate;
  const QString annotate_tip =
      has_pages && !can_annotate ? tr("This document does not allow annotations") : QString();
  // 도구는 권한으로만 켜고 끈다. 바쁨·저장·인쇄 중에는 요청(annotation_edits_allowed_)만 막아
  // 도구가 유지된다 (리뷰 W1)
  annotation_edits_allowed_ = idle && can_annotate;
  for (QAction* action : tool_actions_) {
    if (action == nullptr) continue;
    const bool select = action == tool_actions_[0];
    action->setEnabled(select || can_annotate);
    action->setStatusTip(select ? QString() : annotate_tip);
  }
  if (!can_annotate &&
      view_->tool() != AnnotationTool::kSelect) {  // 권한을 잃으면(문서 교체) 선택 도구로
    tool_actions_[0]->setChecked(true);
    view_->set_tool(AnnotationTool::kSelect);
  }
  for (QAction* action : markup_selection_actions_) {
    if (action != nullptr) action->setEnabled(idle && can_annotate && view_->has_selection());
  }
  delete_annotation_action_->setEnabled(
      (annotation_edits_allowed_ && view_->selected_annotation().has_value()) ||
      (content_edits_allowed_ && view_->selected_object().has_value() &&
       !view_->selected_object_locked()) ||
      (content_edits_allowed_ && view_->selected_line().has_value() &&
       view_->selected_line()->info.editable));
  color_button_->setEnabled(can_annotate);
  // 텍스트 추가는 내용 수정 권한(비트 4) + 번들 폰트. 바쁨·저장·인쇄 중 요청은
  // annotation_edits_allowed_ 와 같은 규칙으로 막는다
  if (QAction* text_tool = tool_actions_[static_cast<int>(AnnotationTool::kText)];
      text_tool != nullptr) {
    const bool can_edit_contents =
        has_pages && session_.handle().info().permissions.modify_contents;
    const bool text_ok = can_edit_contents && text_font_ != nullptr;
    text_edits_allowed_ = idle && text_ok;
    content_edits_allowed_ = idle && can_edit_contents;
    if (paste_action_ != nullptr) paste_action_->setEnabled(can_edit_contents);
    view_->set_text_editable(content_edits_allowed_);  // 이동·크기 조절 (글꼴과 무관)
    view_->set_nudge_enabled(can_edit_contents);       // 화살표는 바쁜 동안에도 받아 모은다
    if (QAction* image_tool = tool_actions_[static_cast<int>(AnnotationTool::kImage)];
        image_tool != nullptr) {
      image_tool->setEnabled(can_edit_contents);
      image_tool->setStatusTip(has_pages && !can_edit_contents
                                   ? tr("This document does not allow content changes")
                                   : QString());
      if (!can_edit_contents && view_->tool() == AnnotationTool::kImage) {
        tool_actions_[0]->setChecked(true);
        view_->set_tool(AnnotationTool::kSelect);
      }
    }
    text_tool->setEnabled(text_ok);
    text_tool->setStatusTip(
        !has_pages              ? QString()
        : !can_edit_contents    ? tr("This document does not allow content changes")
        : text_font_ == nullptr ? tr("Font file is missing (python3 third_party/fetch_fonts.py)")
                                : QString());
    if (text_size_combo_ != nullptr) text_size_combo_->setEnabled(text_ok);
    if (!text_ok && view_->tool() == AnnotationTool::kText) {
      tool_actions_[0]->setChecked(true);
      view_->set_tool(AnnotationTool::kSelect);
    }
  }
  if (width_button_ != nullptr) {
    const auto selected = view_->selected_annotation();
    const bool width_ok = can_annotate && (!selected || has_border_width(selected->kind));
    width_button_->setEnabled(width_ok);
    opacity_button_->setEnabled(can_annotate);
    width_menu_->menuAction()->setEnabled(width_ok);
    opacity_menu_->menuAction()->setEnabled(can_annotate);
  }
  const bool structure_busy = structure_job_ != nullptr && structure_job_->active();
  split_action_->setEnabled(has_pages && !structure_busy);
  merge_action_->setEnabled(!structure_busy);
  // 실행 취소 대상은 이미 허용된 편집이므로 작업 중이 아니면 된다
  undo_action_->setEnabled(idle && edit_->can_undo());
  redo_action_->setEnabled(idle && edit_->can_redo());
  undo_action_->setText(edit_->can_undo() ? tr("&Undo %1").arg(command_label(edit_->undo_id()))
                                          : tr("&Undo"));
  redo_action_->setText(edit_->can_redo() ? tr("&Redo %1").arg(command_label(edit_->redo_id()))
                                          : tr("&Redo"));
  save_action_->setEnabled(session_.has_document() && edit_->is_modified() && !saving_ &&
                           !edit_->busy());
  save_as_action_->setEnabled(has_pages && !saving_ && !edit_->busy());
}

void MainWindow::rotate_current_page(int quarter_turns) {
  rotate_pages(target_pages(), quarter_turns);
}

std::vector<int> MainWindow::target_pages() const {
  if (!view_->has_document()) return {};
  if (thumbnail_panel_->list_has_focus()) {
    std::vector<int> selected = thumbnail_panel_->selected_pages();
    if (!selected.empty()) return selected;
  }
  return {std::max(0, view_->current_page())};
}

bool MainWindow::rotate_pages(std::vector<int> pages, int quarter_turns) {
  if (!rotate_cw_action_->isEnabled() || !view_->has_document() || pages.empty()) return false;
  return edit_->run(std::make_shared<edit::RotatePageCommand>(std::move(pages), quarter_turns));
}

bool MainWindow::warn_signature_once() {
  if (signature_warned_ || session_.handle().info().signature_count <= 0) return true;
  QMessageBox box(QMessageBox::Warning, tr("Signed Document"),
                  tr("This document has a digital signature. Changing pages or annotations may "
                     "invalidate it.\nContinue?"),
                  QMessageBox::NoButton, this);
  box.setObjectName(QStringLiteral("signature_warning"));
  QPushButton* go = box.addButton(tr("Continue"), QMessageBox::AcceptRole);
  QPushButton* cancel = box.addButton(tr("Cancel"), QMessageBox::RejectRole);
  box.setDefaultButton(cancel);
  box.exec();
  if (box.clickedButton() != go) return false;
  signature_warned_ = true;
  return true;
}

bool MainWindow::delete_pages(std::vector<int> pages) {
  if (!delete_pages_action_->isEnabled() || !view_->has_document()) return false;
  pages = edit::normalize_pages(std::move(pages));
  if (pages.empty()) return false;
  if (static_cast<int>(pages.size()) >= session_.handle().info().page_count) {
    show_transient(tr("A document must have at least one page"));
    return false;
  }
  if (!warn_signature_once()) return false;
  return edit_->run(std::make_shared<edit::DeletePagesCommand>(std::move(pages)));
}

bool MainWindow::move_pages(std::vector<int> pages, int drop_row) {
  if (!move_up_action_->isEnabled() || !view_->has_document()) return false;
  pages = edit::normalize_pages(std::move(pages));
  const int count = session_.handle().info().page_count;
  if (pages.empty() || drop_row < 0 || drop_row > count) return false;
  // 썸네일 행 기준 자리 → 블록을 뺀 뒤의 삽입 자리
  int dest = drop_row;
  for (int p : pages) {
    if (p < drop_row) --dest;
  }
  if (!edit::validate_move(count, pages, dest)) return false;
  // 자리가 그대로인 이동은 편집으로 치지 않는다
  const std::vector<int> order = edit::simulate_move(count, pages, dest);
  bool identity = true;
  for (std::size_t i = 0; i < order.size(); ++i)
    identity = identity && order[i] == static_cast<int>(i);
  if (identity) return false;
  if (!warn_signature_once()) return false;
  return edit_->run(std::make_shared<edit::MovePagesCommand>(std::move(pages), dest));
}

void MainWindow::move_selection(int delta) {
  std::vector<int> pages = edit::normalize_pages(target_pages());
  if (pages.empty()) return;
  const int count = session_.handle().info().page_count;
  if (delta < 0 && pages.front() == 0) return;
  if (delta > 0 && pages.back() >= count - 1) return;
  // 블록을 한 자리 위/아래로: 행 기준 자리는 첫 쪽 − 1 / 마지막 쪽 + 2
  move_pages(pages, delta < 0 ? pages.front() - 1 : pages.back() + 2);
}

void MainWindow::on_insert_pages_action() {
  if (!insert_pages_action_->isEnabled() || !view_->has_document()) return;
  const QString path = QFileDialog::getOpenFileName(
      this, tr("PDF to Insert Pages From"),
      start_dir(DirKind::kInsert, QFileInfo(session_.path()).absolutePath()),
      tr("PDF documents (*.pdf)"));
  if (path.isEmpty()) return;
  remember_dir(DirKind::kInsert, path);
  insert_pages_from(path, std::max(0, view_->current_page()) + 1);  // 현재 쪽 뒤
}

bool MainWindow::insert_pages_from(const QString& path, int at, const QString& password) {
  if (!insert_pages_action_->isEnabled() || !view_->has_document() || path.isEmpty()) return false;
  const int count = session_.handle().info().page_count;
  at = std::clamp(at, 0, count);
  if (!warn_signature_once()) return false;
  pending_insert_ = PendingInsert{path, at};
  const QByteArray utf8 = path.toUtf8();
  return edit_->run(std::make_shared<edit::InsertPagesCommand>(
      onpdf::path_from_utf8(std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()))),
      password.toStdString(), std::vector<int>{}, at));
}

void MainWindow::on_edit_failed(onpdf::ErrorCode code, const QString& message) {
  if (pending_image_) {
    const PendingImage pending = *pending_image_;
    pending_image_.reset();
    last_image_command_.reset();
    // PDFium 이 넣지 못한 JPEG(CMYK·일부 프로그레시브 등)은 디코딩해 픽셀로 다시 넣는다
    if (pending.jpeg && code == ErrorCode::kInvalidFormat && !pending.path.isEmpty()) {
      if (auto imported = import_image_file(pending.path, true)) {
        const engine::PageSize size =
            session_.handle().info().page_sizes.at(static_cast<std::size_t>(pending.page));
        imported->spec.rect = place_image(size, pending.origin, imported->pixel_width,
                                          imported->pixel_height, pending.center);
        pending_image_ =
            PendingImage{pending.path, pending.page, pending.origin, pending.center, false};
        if (run_add_image(pending.page, std::move(imported->spec))) return;
      }
    }
  }
  if (pending_insert_ && code == ErrorCode::kPasswordRequired) {
    // 삽입 소스가 암호 문서: 묻고 같은 삽입을 다시 시도한다 (취소하면 끝)
    const PendingInsert pending = *pending_insert_;
    pending_insert_.reset();
    QInputDialog dialog(this);
    dialog.setObjectName(QStringLiteral("insert_password"));
    dialog.setWindowTitle(tr("Password"));
    dialog.setLabelText(tr("%1 is password protected. Enter the password.")
                            .arg(QFileInfo(pending.path).fileName()));
    dialog.setTextEchoMode(QLineEdit::Password);
    if (dialog.exec() != QDialog::Accepted || dialog.textValue().isEmpty()) return;
    insert_pages_from(pending.path, pending.at, dialog.textValue());
    return;
  }
  pending_insert_.reset();
  if (code == ErrorCode::kPermissionDenied) {
    show_transient(
        tr("This document does not allow page extraction, so its pages cannot be inserted"));
    return;
  }
  // 줄 수정(#65)의 거절 사유는 사용자 문구로
  if (message.startsWith(QStringLiteral("displacement"))) {
    show_transient(tr("This line cannot be edited because text continues it on the same line"));
    return;
  }
  if (message.startsWith(QStringLiteral("shared-form"))) {
    show_transient(tr("Text inside a form shared by several pages cannot be edited"));
    return;
  }
  if (message.startsWith(QStringLiteral("content stream does not match"))) {
    show_transient(
        tr("The content structure of this document cannot be read, so the line cannot be edited"));
    return;
  }
  if (message.startsWith(QStringLiteral("line edit changed other content"))) {
    show_transient(tr("The line edit was cancelled because other content could change"));
    return;
  }
  if (message.startsWith(QStringLiteral("undo history evicted"))) {
    show_transient(tr("Undo history was cleared to stay within the memory limit"));
    return;
  }
  show_transient(tr("Could not edit: %1").arg(message));
}

// ---------------------------------------------------------------- 주석

namespace {

QIcon color_icon(engine::Color c) {
  QPixmap pm(16, 16);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setPen(QPen(Qt::darkGray, 1));
  p.setBrush(QColor(c.r, c.g, c.b));
  p.drawRect(1, 1, 13, 13);
  return QIcon(pm);
}

QString kind_name(engine::AnnotationKind kind) {
  switch (kind) {
    case engine::AnnotationKind::kHighlight:
      return MainWindow::tr("Highlight");
    case engine::AnnotationKind::kUnderline:
      return MainWindow::tr("Underline");
    case engine::AnnotationKind::kStrikeOut:
      return MainWindow::tr("Strikeout");
    case engine::AnnotationKind::kSquiggly:
      return MainWindow::tr("Squiggly");
    case engine::AnnotationKind::kText:
      return MainWindow::tr("Note");
    case engine::AnnotationKind::kInk:
      return MainWindow::tr("Pen");
    case engine::AnnotationKind::kSquare:
      return MainWindow::tr("Rectangle");
    case engine::AnnotationKind::kCircle:
      return MainWindow::tr("Ellipse");
    case engine::AnnotationKind::kLink:
      return MainWindow::tr("Link");
    case engine::AnnotationKind::kOther:
      break;
  }
  return MainWindow::tr("Annotation");
}

// PDF 날짜 "D:YYYYMMDDHHmmSS…" → "YYYY-MM-DD"
QString short_date(const std::string& pdf_date) {
  if (pdf_date.size() < 10 || pdf_date.rfind("D:", 0) != 0) return QString::fromStdString(pdf_date);
  const QString d = QString::fromStdString(pdf_date.substr(2, 8));
  return d.left(4) + QLatin1Char('-') + d.mid(4, 2) + QLatin1Char('-') + d.mid(6, 2);
}

}  // namespace

void MainWindow::build_annotation_ui() {
  annotation_toolbar_ = addToolBar(tr("Annotation"));
  annotation_toolbar_->setObjectName(QStringLiteral("annotation_toolbar"));
  annotation_toolbar_->setMovable(false);
  annotation_toolbar_->setToolButtonStyle(Qt::ToolButtonTextOnly);
  QMenu* menu = menuBar()->addMenu(tr("A&nnotate"));
  tool_group_ = new QActionGroup(this);
  tool_group_->setExclusive(true);
  const QString names[kAnnotationToolCount] = {
      tr("&Select"), tr("&Highlight"), tr("&Underline"), tr("Stri&keout"), tr("&Note"),
      tr("&Pen"),    tr("&Rectangle"), tr("&Ellipse"),   tr("&Text"),      tr("Ima&ge")};
  const QString tips[kAnnotationToolCount] = {
      tr("Select text or pick an annotation"),
      tr("Highlight the dragged text"),
      tr("Underline the dragged text"),
      tr("Strike out the dragged text"),
      tr("Note at the clicked spot"),
      tr("Freehand line by dragging"),
      tr("Rectangle around the dragged area"),
      tr("Ellipse in the dragged area"),
      tr("Text box at the clicked spot (becomes page content)"),
      tr("Choose a file and place the image at the clicked spot")};
  for (int i = 0; i < kAnnotationToolCount; ++i) {
    QAction* action = tool_group_->addAction(names[i]);
    action->setCheckable(true);
    action->setStatusTip(tips[i]);
    action->setChecked(i == 0);
    connect(action, &QAction::triggered, this, [this, i](bool checked) {
      if (!checked) return;
      view_->set_tool(static_cast<AnnotationTool>(i));
      if (static_cast<AnnotationTool>(i) == AnnotationTool::kText)
        ensure_font_catalog();  // 줄 수정 전에 미리 (#73)
    });
    tool_actions_[i] = action;
    annotation_toolbar_->addAction(action);
    menu->addAction(action);
  }
  annotation_toolbar_->addSeparator();
  menu->addSeparator();
  color_button_ = new QToolButton(annotation_toolbar_);
  color_button_->setObjectName(QStringLiteral("annotation_color"));
  color_button_->setToolTip(tr("Annotation color"));
  color_button_->setPopupMode(QToolButton::InstantPopup);
  auto* palette = new QMenu(color_button_);
  QMenu* color_menu = menu->addMenu(tr("&Color"));
  for (int i = 0; i < kPaletteSize; ++i) {
    const engine::Color c = kPalette[i];
    for (QMenu* m : {palette, color_menu}) {
      QAction* a = m->addAction(color_icon(c), tr(kPaletteNames[i]));
      connect(a, &QAction::triggered, this, [this, c] { set_annotation_color(c); });
    }
  }
  color_button_->setMenu(palette);
  update_color_icon();
  annotation_toolbar_->addWidget(color_button_);
  // 두께 ▾ (도형·펜) · 불투명도 ▾ — 선택이 있으면 그 주석, 없으면 새 도형·펜의 기본값 (색 버튼과
  // 같은 규칙)
  width_button_ = new QToolButton(annotation_toolbar_);
  width_button_->setObjectName(QStringLiteral("annotation_width"));
  width_button_->setToolTip(tr("Line width (shapes and pen)"));
  width_button_->setPopupMode(QToolButton::InstantPopup);
  auto* width_popup = new QMenu(width_button_);
  QMenu* width_menu = width_menu_ = menu->addMenu(tr("&Width"));
  for (double w : {1.0, 2.0, 4.0, 8.0}) {
    for (QMenu* m : {width_popup, width_menu}) {
      QAction* a = m->addAction(tr("%1 pt").arg(w));
      a->setData(w);
      connect(a, &QAction::triggered, this, [this, w] { set_annotation_border_width(w); });
    }
  }
  width_button_->setMenu(width_popup);
  annotation_toolbar_->addWidget(width_button_);
  opacity_button_ = new QToolButton(annotation_toolbar_);
  opacity_button_->setObjectName(QStringLiteral("annotation_opacity"));
  opacity_button_->setToolTip(tr("Opacity"));
  opacity_button_->setPopupMode(QToolButton::InstantPopup);
  auto* opacity_popup = new QMenu(opacity_button_);
  QMenu* opacity_menu = opacity_menu_ = menu->addMenu(tr("&Opacity"));
  for (int percent : {25, 50, 75, 100}) {
    const auto alpha = static_cast<std::uint8_t>((percent * 255 + 50) / 100);
    for (QMenu* m : {opacity_popup, opacity_menu}) {
      QAction* a = m->addAction(tr("%1%").arg(percent));
      a->setData(percent);
      connect(a, &QAction::triggered, this, [this, alpha] { set_annotation_opacity(alpha); });
    }
  }
  opacity_button_->setMenu(opacity_popup);
  annotation_toolbar_->addWidget(opacity_button_);
  // 텍스트 크기 (텍스트 도구 전용)
  text_size_combo_ = new QComboBox(annotation_toolbar_);
  text_size_combo_->setObjectName(QStringLiteral("text_size"));
  text_size_combo_->setToolTip(tr("Text size"));
  for (int pt : {8, 10, 12, 14, 18, 24, 36, 48, 72})
    text_size_combo_->addItem(tr("%1 pt").arg(pt), pt);
  text_size_combo_->setCurrentIndex(5);  // 24pt
  connect(text_size_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
    if (index < 0 || syncing_text_size_) return;
    set_text_size(text_size_combo_->itemData(index).toDouble());
  });
  annotation_toolbar_->addWidget(text_size_combo_);
  update_property_buttons();
  annotation_toolbar_->addSeparator();
  delete_annotation_action_ = annotation_toolbar_->addAction(
      tr("Delete Annotation"), this, [this] { delete_selected_annotation(); });
  delete_annotation_action_->setStatusTip(tr("Delete the selected annotation (Delete)"));
  delete_annotation_action_->setEnabled(false);
  menu->addAction(delete_annotation_action_);
  menu->addSeparator();
  const engine::AnnotationKind kinds[3] = {engine::AnnotationKind::kHighlight,
                                           engine::AnnotationKind::kUnderline,
                                           engine::AnnotationKind::kStrikeOut};
  const QString sel_names[3] = {tr("Highlight Selection"), tr("Underline Selection"),
                                tr("Strike Out Selection")};
  const QKeySequence keys[3] = {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H),
                                QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_U),
                                QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K)};
  for (int i = 0; i < 3; ++i) {
    const engine::AnnotationKind kind = kinds[i];
    markup_selection_actions_[i] = menu->addAction(
        sel_names[i], keys[i], this, [this, kind] { add_markup_from_selection(kind); });
    markup_selection_actions_[i]->setEnabled(false);
  }
}

QAction* MainWindow::tool_action(AnnotationTool tool) const {
  return tool_actions_[static_cast<int>(tool)];
}

void MainWindow::update_color_icon() {
  color_button_->setIcon(color_icon(annotation_color_));
  color_button_->setText(tr("Color"));
}

void MainWindow::update_property_buttons() {
  if (text_size_combo_ != nullptr) {
    // 선택한 텍스트 상자의 크기를 콤보에 비춘다 (선택이 없으면 기본값)
    const auto selected = view_->selected_text_box();
    const auto line = view_->selected_line();
    const double shown = line ? line->info.font_size : selected ? selected->font_size : text_size_;
    const int index = text_size_combo_->findData(static_cast<int>(std::lround(shown)));
    if (index >= 0 && index != text_size_combo_->currentIndex()) {
      QScopedValueRollback<bool> guard(syncing_text_size_, true);
      text_size_combo_->setCurrentIndex(index);
    }
  }
  if (width_button_ == nullptr || opacity_button_ == nullptr) return;
  // 선택이 있으면 그 주석의 값, 없으면 새 도형·펜의 기본값
  if (auto selected = view_->selected_annotation(); selected) {
    const bool unknown = selected->has_bs && !selected->has_border;  // /BS /W 는 읽을 수 없다
    width_button_->setText(has_border_width(selected->kind) && !unknown
                               ? width_text(selected->has_border ? selected->border_width : 1.0)
                               : tr("Width"));
    opacity_button_->setText(opacity_text(selected->opacity));
  } else {
    width_button_->setText(width_text(shape_border_width_));
    opacity_button_->setText(opacity_text(shape_alpha_ / 255.0));
  }
}

void MainWindow::set_annotation_border_width(double width) {
  if (auto selected = view_->selected_annotation(); selected) {
    if (!annotation_edits_allowed_ || !has_border_width(selected->kind) || !warn_signature_once())
      return;
    const bool recreate = selected->has_bs;
    if (edit_->run(std::make_shared<edit::SetAnnotationBorderWidthCommand>(
            view_->selected_annotation_page(), selected->id, width))) {
      show_transient(recreate ? tr("The annotation was made by another program, so it was "
                                   "recreated (undo may not restore the original width)")
                              : tr("Changed the width of the selected annotation"));
    }
    return;
  }
  shape_border_width_ = width;
  settings_->set_border_width(width);  // (#129) — changed → apply_settings 는 같은 값이라 no-op
  update_property_buttons();
}

void MainWindow::set_annotation_opacity(std::uint8_t alpha) {
  if (auto selected = view_->selected_annotation(); selected) {
    if (!annotation_edits_allowed_ || !warn_signature_once()) return;
    if (edit_->run(std::make_shared<edit::SetAnnotationOpacityCommand>(
            view_->selected_annotation_page(), selected->id, alpha))) {
      show_transient(tr("Changed the opacity of the selected annotation"));
    }
    return;
  }
  shape_alpha_ = alpha;
  settings_->set_opacity_percent((alpha * 100 + 127) / 255);  // (#129)
  update_property_buttons();
}

std::string MainWindow::annotation_author() const {
  // OS 로그인 이름은 개인 정보라 파일에 쓰지 않는다. 설정의 작성자 이름(#129, 기본 빈 값)만 쓴다
  return settings_->author().toStdString();
}

void MainWindow::set_annotation_color(engine::Color color) {
  // 주석이 선택돼 있으면 그 주석의 색만 바꾼다 (다음 주석의 기본 색은 그대로). 선택이 없을 때만
  // 기본 색을 바꾼다
  if (auto selected = view_->selected_annotation(); selected) {
    if (!annotation_edits_allowed_ || !warn_signature_once()) return;
    if (edit_->run(std::make_shared<edit::SetAnnotationColorCommand>(
            view_->selected_annotation_page(), selected->id, color))) {
      show_transient(
          tr("Changed the color of the selected annotation (the default color for new annotations "
             "is unchanged)"));
    }
    return;
  }
  if (auto line = view_->selected_line();
      line) {  // 기존 줄의 색만 (#69). 불가 줄은 안내만 (기본값도 그대로)
    if (!line->info.editable) {
      show_transient(line_reason_text(line->info));
      return;
    }
    if (line->info.color.r == color.r && line->info.color.g == color.g &&
        line->info.color.b == color.b)
      return;
    run_line_replace(
        line->page, line->info.id, std::nullopt, std::nullopt, color,
        fallback_font_for(line->info.font_name, QString::fromStdString(line->info.text)));
    return;
  }
  if (auto selected = view_->selected_text_box(); selected) {  // 선택한 텍스트 상자의 색만
    if (selected->color.r == color.r && selected->color.g == color.g &&
        selected->color.b == color.b)
      return;
    if (selected->original_font)
      run_line_replace(
          selected->page, selected->box, std::nullopt, std::nullopt, color,
          fallback_font_for(selected->font_name,
                            QString::fromStdString(selected->text)));  // 원본 글꼴 유지
    else
      replace_selected_text(std::nullopt, std::nullopt, color);
    return;
  }
  annotation_color_ = color;
  text_color_ = color;                     // 새 텍스트도 같은 색 (기본값만 다르다: 텍스트는 검정)
  settings_->set_annotation_color(color);  // (#129)
  settings_->set_text_color(color);
  update_color_icon();
}

bool MainWindow::add_markup_from_selection(engine::AnnotationKind kind) {
  if (!annotation_edits_allowed_ || !view_->has_document()) return false;
  const MarkupItems items = view_->selection_markup_items();
  if (items.empty()) return false;
  on_markup_requested(kind, items);
  return true;
}

void MainWindow::on_markup_requested(engine::AnnotationKind kind, const MarkupItems& items) {
  if (!annotation_edits_allowed_ || items.empty()) return;
  if (!warn_signature_once()) return;
  std::vector<std::pair<int, engine::AnnotationSpec>> specs;
  for (const auto& [page, rects] : items) {
    engine::AnnotationSpec spec;
    spec.kind = kind;
    spec.quads = rects;
    spec.color = annotation_color_;
    spec.flags = engine::kAnnotFlagPrint;
    spec.author = annotation_author();
    specs.emplace_back(page, std::move(spec));
  }
  edit_->run(std::make_shared<edit::AddAnnotationsCommand>(std::move(specs)));
}

void MainWindow::on_note_requested(int page, onpdf::Point pt) {
  if (!annotation_edits_allowed_) return;
  AnnotationDialog dialog(tr("Note"), QString(), this);
  if (dialog.exec() != QDialog::Accepted) return;
  const QString text = dialog.text().trimmed();
  if (text.isEmpty()) return;  // 빈 메모는 만들지 않는다
  add_note(page, pt, text);
}

bool MainWindow::add_note(int page, onpdf::Point pt, const QString& contents) {
  if (!annotation_edits_allowed_ || !view_->has_document()) return false;
  if (!warn_signature_once()) return false;
  engine::AnnotationSpec spec;
  spec.kind = engine::AnnotationKind::kText;
  spec.rect = {pt.x - 10, pt.y - 10, 20, 20};
  spec.flags = engine::kAnnotFlagPrint | engine::kAnnotFlagNoZoom | engine::kAnnotFlagNoRotate;
  spec.color = annotation_color_;
  spec.contents = contents.toStdString();
  spec.author = annotation_author();
  return edit_->run(std::make_shared<edit::AddAnnotationsCommand>(
      std::vector<std::pair<int, engine::AnnotationSpec>>{{page, std::move(spec)}}));
}

// ---------------------------------------------------------------- 텍스트 추가 (#45)

void MainWindow::load_bundled_text_font() {
  // 빌드가 third_party/fonts/ 의 Noto Sans KR 을 실행 파일 옆 fonts/ 로 복사한다 (fetch_fonts.py).
  // 없으면 도구 비활성
  const QString dir = QCoreApplication::applicationDirPath();
  for (const QString& candidate :
       {dir + QStringLiteral("/fonts/NotoSansKR[wght].ttf"),
        dir + QStringLiteral("/../Resources/fonts/NotoSansKR[wght].ttf")}) {
    if (QFileInfo::exists(candidate) && set_text_font(candidate)) return;
  }
}

void MainWindow::set_font_catalog(std::shared_ptr<const edit::FontCatalog> catalog) {
  font_catalog_ = std::move(catalog);
  fallback_cache_.clear();
}

void MainWindow::ensure_font_catalog() {
  if (font_catalog_ != nullptr || font_scan_started_) return;
  font_scan_started_ = true;
  catalog_loader_->start(platform::system_font_dirs());
}

std::shared_ptr<const edit::FontFace> MainWindow::load_catalog_font(const edit::FontEntry& entry) {
  const std::string key = onpdf::path_to_utf8(entry.path) + "#" + std::to_string(entry.face_index);
  for (auto it = fallback_cache_.begin(); it != fallback_cache_.end(); ++it) {
    if (it->first != key) continue;
    auto face = it->second;
    fallback_cache_.erase(it);
    fallback_cache_.emplace_back(key, face);  // 최근 사용을 뒤로
    return face;
  }
  auto face = edit::FontFace::load(entry.path, entry.face_index);
  if (!face) return nullptr;
  fallback_cache_.emplace_back(key, *face);
  if (fallback_cache_.size() > 8) fallback_cache_.erase(fallback_cache_.begin());
  return *face;
}

std::shared_ptr<const edit::FontFace> MainWindow::fallback_font_for(
    const std::string& base_font_name, const QString& text) {
  if (font_catalog_ == nullptr) {
    ensure_font_catalog();  // 끝나기 전에는 번들
    return text_font_;
  }
  const bool allow_cff = session_.handle().info().signature_count == 0;
  const std::string utf8 = text.toUtf8().toStdString();
  std::vector<const edit::FontEntry*> candidates = font_catalog_->match(base_font_name, allow_cff);
  bool hangul = false;
  for (const QChar ch : text) {
    const ushort u = ch.unicode();
    if ((u >= 0xAC00 && u <= 0xD7A3) || (u >= 0x1100 && u <= 0x11FF) ||
        (u >= 0x3130 && u <= 0x318F))
      hangul = true;
  }
  if (hangul) {
    if (const edit::FontEntry* sans = font_catalog_->default_hangul_sans(allow_cff);
        sans != nullptr)
      candidates.push_back(sans);
  }
  for (const edit::FontEntry* entry : candidates) {
    auto face = load_catalog_font(*entry);
    if (face != nullptr && face->missing(utf8).empty()) return face;
  }
  return text_font_;
}

bool MainWindow::set_text_font(const QString& path) {
  auto face = edit::FontFace::load(onpdf::path_from_utf8(path.toUtf8().toStdString()));
  if (!face) {
    text_font_.reset();
    text_font_path_.clear();
    update_edit_actions();
    return false;
  }
  text_font_ = *face;
  text_font_path_ = path;
  update_edit_actions();
  return true;
}

void MainWindow::set_text_size(double size) {
  // 기존 줄(텍스트 도구)이 선택돼 있으면 그 줄의 크기만 (#69)
  if (auto line = view_->selected_line(); line) {
    if (!line->info.editable) {  // 불가 줄은 안내만 (기본값도 그대로, 콤보는 되돌린다)
      show_transient(line_reason_text(line->info));
      update_property_buttons();
      return;
    }
    if (std::abs(line->info.font_size - size) < 0.01) return;
    run_line_replace(
        line->page, line->info.id, std::nullopt, size, std::nullopt,
        fallback_font_for(line->info.font_name, QString::fromStdString(line->info.text)));
    return;
  }
  // 텍스트 상자가 선택돼 있으면 그 상자의 크기만 바꾼다 (주석 색 버튼과 같은 규칙). 없으면 새
  // 텍스트의 기본값
  if (auto selected = view_->selected_text_box(); selected && !view_->selected_annotation()) {
    if (std::abs(selected->font_size - size) < 0.01) return;
    if (selected->original_font)
      run_line_replace(
          selected->page, selected->box, std::nullopt, size, std::nullopt,
          fallback_font_for(selected->font_name,
                            QString::fromStdString(selected->text)));  // 원본 글꼴 유지
    else
      replace_selected_text(std::nullopt, size, std::nullopt);
    return;
  }
  text_size_ = size;
  settings_->set_text_size(static_cast<int>(std::lround(size)));  // (#129)
  sync_text_size_combo();
}

void MainWindow::on_text_requested(int page, onpdf::Point pt) {
  if (!text_edits_allowed_ || text_font_ == nullptr) return;
  for (;;) {
    AnnotationDialog dialog(tr("Text"), QString(), this);
    dialog.setObjectName(QStringLiteral("text_input"));
    if (dialog.exec() != QDialog::Accepted) return;
    const QString text = dialog.text().normalized(
        QString::NormalizationForm_C);     // NFD 입력(macOS 파일명 등)은 NFC 로 (리뷰 W3)
    if (text.trimmed().isEmpty()) return;  // 빈 텍스트는 만들지 않는다
    const std::u32string missing = text_font_->missing(text.toUtf8().toStdString());
    if (missing.empty()) {
      add_text(page, pt, text);
      return;
    }
    QMessageBox box(QMessageBox::Warning, tr("Text"),
                    tr("Some characters are not in the font: %1\nPlease use other characters.")
                        .arg(QString::fromStdU32String(missing.substr(0, 10))),
                    QMessageBox::Ok, this);
    box.setObjectName(QStringLiteral("text_missing_glyphs"));
    box.exec();
  }
}

bool MainWindow::add_text(int page, onpdf::Point pt, const QString& utf8) {
  if (!text_edits_allowed_ || !view_->has_document() || text_font_ == nullptr) return false;
  if (utf8.trimmed().isEmpty()) return false;
  if (!warn_signature_once()) return false;
  edit::TextBoxSpec spec;
  spec.page = page;
  spec.origin = pt;
  spec.size = text_size_;
  spec.color = text_color_;
  spec.color.a = 255;
  spec.utf8 = utf8.normalized(QString::NormalizationForm_C).toUtf8().toStdString();
  spec.face = text_font_;
  return edit_->run(std::make_shared<edit::AddTextCommand>(std::move(spec)));
}

// ---------------------------------------------------------------- 텍스트 상자 편집 (#47)

void MainWindow::on_text_box_selected(bool has, const DocumentView::SelectedTextBox& box) {
  update_edit_actions();
  update_property_buttons();
  if (!has) {
    if (selection_status_shown_) show_transient(QString());
    return;
  }
  if (!pending_line_notice_.isEmpty()) {  // 줄 수정 뒤 새 상자 선택: 선택 문구 대신 안내 (#65)
    statusBar()->showMessage(std::exchange(pending_line_notice_, QString()), 6000);
    selection_status_shown_ = true;
    return;
  }
  QStringList parts;
  if (box.kind == engine::ObjectKind::kImage) {
    parts << tr("Image") << tr("%1×%2 px").arg(box.pixel_width).arg(box.pixel_height)
          << tr("%1×%2 pt").arg(qRound(box.bbox.w)).arg(qRound(box.bbox.h));
    if (!box.filter.empty())
      parts << QString::fromStdString(box.filter == "DCTDecode" ? "JPEG" : box.filter);
  } else {
    parts << tr("Text") << tr("%1 pt").arg(box.font_size)
          << tr("%n line(s)", nullptr, static_cast<int>(box.lines));
    if (!box.text.empty())
      parts << QString::fromStdString(box.text).left(60).replace(QLatin1Char('\n'),
                                                                 QLatin1Char(' '));
  }
  statusBar()->showMessage(parts.join(QStringLiteral(" · ")));
  selection_status_shown_ = true;
}

namespace {
// 마크 없는 기존 이미지 id ("#obj<index>") — 편집하면 원본 스트림이 재생성된다 (#57)
bool has_foreign_object(const std::vector<std::string>& ids) {
  return std::any_of(ids.begin(), ids.end(),
                     [](const std::string& id) { return id.starts_with("#obj"); });
}
}  // namespace

void MainWindow::on_text_move_requested(int page, const std::vector<std::string>& ids,
                                        onpdf::Point delta) {
  if (!content_edits_allowed_ || ids.empty()) return;
  if (has_foreign_object(ids)) return explain_locked_object();
  if (!warn_signature_once()) return;
  edit_->run(std::make_shared<edit::MoveObjectsCommand>(page, ids, delta));
}

void MainWindow::on_text_delete_requested(int, const QString&) {
  delete_selected_text_box();
}

// ---------------------------------------------------------------- 이미지 (#49)

void MainWindow::on_image_requested(int page, onpdf::Point pt) {
  if (!content_edits_allowed_) return;
  const QString path = QFileDialog::getOpenFileName(
      this, tr("Insert Image"),
      start_dir(DirKind::kImage, QFileInfo(session_.path()).absolutePath()), image_file_filter());
  if (path.isEmpty()) return;
  remember_dir(DirKind::kImage, path);
  add_image_file(path, page, pt, false);
}

bool MainWindow::add_image_file(const QString& path, int page, onpdf::Point origin, bool center) {
  if (!content_edits_allowed_ || !view_->has_document()) return false;
  auto imported = import_image_file(path);
  if (!imported) {
    QMessageBox box(
        QMessageBox::Warning, tr("Image"),
        tr("Cannot open this image: %1\n%2")
            .arg(QFileInfo(path).fileName(), QString::fromStdString(imported.error().message)),
        QMessageBox::Ok, this);
    box.setObjectName(QStringLiteral("image_open_failed"));
    box.exec();
    return false;
  }
  const engine::PageSize size =
      session_.handle().info().page_sizes.at(static_cast<std::size_t>(page));
  imported->spec.rect =
      place_image(size, origin, imported->pixel_width, imported->pixel_height, center);
  pending_image_ = PendingImage{path, page, origin, center,
                                std::holds_alternative<engine::JpegBytes>(imported->spec.source)};
  return run_add_image(page, std::move(imported->spec));
}

bool MainWindow::add_image(const QImage& image, int page, onpdf::Point origin, bool center) {
  if (!content_edits_allowed_ || !view_->has_document()) return false;
  auto imported = import_image(image);
  if (!imported) {
    show_transient(
        tr("Cannot insert this image: %1").arg(QString::fromStdString(imported.error().message)));
    return false;
  }
  const engine::PageSize size =
      session_.handle().info().page_sizes.at(static_cast<std::size_t>(page));
  imported->spec.rect =
      place_image(size, origin, imported->pixel_width, imported->pixel_height, center);
  pending_image_ = PendingImage{QString(), page, origin, center, false};
  return run_add_image(page, std::move(imported->spec));
}

bool MainWindow::run_add_image(int page, engine::ImageSpec spec) {
  if (!warn_signature_once()) return false;
  auto command = std::make_shared<edit::AddImageCommand>(page, std::move(spec));
  if (!edit_->run(command)) {
    pending_image_.reset();
    return false;
  }
  last_image_command_ = std::move(command);
  return true;
}

void MainWindow::on_object_nudge_requested(int page, const std::vector<std::string>& ids,
                                           onpdf::Point delta) {
  if (!view_->has_document() || !session_.handle().info().permissions.modify_contents) return;
  if (has_foreign_object(ids)) return explain_locked_object();
  if (pending_nudge_ && (pending_nudge_->page != page || pending_nudge_->ids != ids))
    pending_nudge_.reset();
  if (!pending_nudge_) pending_nudge_ = PendingNudge{page, ids, {0.0, 0.0}};
  pending_nudge_->delta.x += delta.x;
  pending_nudge_->delta.y += delta.y;
  flush_pending_nudge();
}

void MainWindow::flush_pending_nudge() {
  if (!pending_nudge_ || !content_edits_allowed_ || edit_->busy())
    return;  // 끝나면 state_changed 에서 다시
  const PendingNudge nudge = std::move(*pending_nudge_);
  pending_nudge_.reset();
  if (nudge.delta.x == 0.0 && nudge.delta.y == 0.0) return;
  if (!warn_signature_once()) return;
  edit_->run(std::make_shared<edit::MoveObjectsCommand>(nudge.page, nudge.ids, nudge.delta, true));
}

// ---------------------------------------------------------------- 폼 채우기 (#51)

void MainWindow::on_form_field_clicked(int page, const engine::FormWidgetInfo& widget) {
  if (!form_edits_allowed_) {
    show_transient(tr("This document does not allow filling forms"));
    return;
  }
  if (widget.read_only) {
    show_transient(tr("Read-only field: %1").arg(QString::fromStdString(widget.ref.name)));
    return;
  }
  statusBar()->showMessage(tr("Form · %1").arg(QString::fromStdString(widget.ref.name)), 4000);
  switch (widget.type) {
    case engine::FieldType::kText:
      view_->open_form_editor(page, widget);
      break;
    case engine::FieldType::kCheckBox:
      run_form_command(
          std::make_shared<edit::SetFormCheckedCommand>(page, widget.ref, !widget.checked));
      break;
    case engine::FieldType::kRadio:
      if (!widget.checked)
        run_form_command(std::make_shared<edit::SetFormCheckedCommand>(page, widget.ref, true));
      break;
    case engine::FieldType::kComboBox:
    case engine::FieldType::kListBox:
      show_form_choice_menu(page, widget);
      break;
    default:
      show_transient(tr("This field cannot be filled"));
      break;
  }
}

void MainWindow::show_form_choice_menu(int page, const engine::FormWidgetInfo& widget) {
  QMenu menu(this);
  menu.setObjectName(QStringLiteral("form_choice_menu"));
  for (int i = 0; i < static_cast<int>(widget.options.size()); ++i) {
    QAction* action =
        menu.addAction(QString::fromStdString(widget.options[static_cast<std::size_t>(i)]));
    action->setCheckable(true);
    action->setChecked(std::find(widget.selected.begin(), widget.selected.end(), i) !=
                       widget.selected.end());
    connect(action, &QAction::triggered, this, [this, page, widget, i] {
      std::vector<int> indices{i};
      if (widget.multi_select) {  // 다중 선택: 누른 항목만 켜고 끈다
        indices = widget.selected;
        if (auto it = std::find(indices.begin(), indices.end(), i); it != indices.end()) {
          indices.erase(it);
        } else {
          indices.push_back(i);
        }
      }
      choose_form_option(page, widget.ref, std::move(indices));
    });
  }
  if (widget.editable_combo) {
    menu.addSeparator();
    QAction* typed = menu.addAction(tr("Enter a value…"));
    connect(typed, &QAction::triggered, this,
            [this, page, widget] { view_->open_form_editor(page, widget); });
  }
  const QRectF rect = view_->page_rect_to_viewport(page, widget.rect);
  menu.exec(view_->viewport()->mapToGlobal(rect.bottomLeft().toPoint()));
}

bool MainWindow::choose_form_option(int page, const engine::FieldRef& ref,
                                    std::vector<int> indices) {
  if (!form_edits_allowed_ || !view_->has_document()) return false;
  std::sort(indices.begin(), indices.end());
  run_form_command(std::make_shared<edit::SetFormChoiceCommand>(page, ref, std::move(indices)));
  return true;
}

void MainWindow::on_form_text_committed(int page, const engine::FormWidgetInfo& widget,
                                        const QString& text) {
  if (!form_edits_allowed_) return;
  const std::string value =
      text.normalized(QString::NormalizationForm_C).toStdString();  // 한글은 NFC 로 (#45 와 같다)
  if (value == widget.value) return;  // 바뀌지 않았으면 Command 를 만들지 않는다
  run_form_command(std::make_shared<edit::SetFormTextCommand>(page, widget.ref, value));
}

void MainWindow::run_form_command(std::shared_ptr<edit::Command> command) {
  pending_form_commands_.push_back(std::move(command));
  flush_form_commands();
}

void MainWindow::flush_form_commands() {
  // 하나씩 순서대로: 앞 Command 가 끝나면(state_changed) 다음을 보낸다
  if (pending_form_commands_.empty() || edit_->busy() || saving_) return;
  auto command = std::move(pending_form_commands_.front());
  pending_form_commands_.pop_front();
  if (edit_->run(command)) {
    form_in_flight_ = true;
  } else {
    pending_form_commands_.clear();  // 문서 없음: 버린다
  }
}

void MainWindow::settle_form_edits() {
  view_->commit_form_editor();
  // 폼 편집만 기다린다 (다른 편집이 바쁜 경우는 호출자가 기존 규칙대로 "잠시 후" 안내)
  if (pending_form_commands_.empty() && !form_in_flight_) return;
  // 폼 Command 는 수 ms 다. 끝날 때까지 이벤트를 돌리며 기다린다 (상한 5초)
  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  const auto check = [this, &loop] {
    if (pending_form_commands_.empty() && !form_in_flight_) loop.quit();
  };
  const QMetaObject::Connection connection =
      connect(edit_, &EditController::state_changed, &loop, check, Qt::QueuedConnection);
  timeout.start(5000);
  QTimer::singleShot(0, &loop, check);
  loop.exec(QEventLoop::ExcludeUserInputEvents);
  disconnect(connection);
}

bool MainWindow::paste_image() {
  if (!content_edits_allowed_ || !view_->has_document()) return false;
  const QMimeData* mime = QApplication::clipboard()->mimeData();
  const int page = std::max(0, view_->current_page());
  const engine::PageSize size =
      session_.handle().info().page_sizes.at(static_cast<std::size_t>(page));
  const onpdf::Point center{size.width / 2, size.height / 2};
  if (mime != nullptr && mime->hasImage()) {
    const QImage image = qvariant_cast<QImage>(mime->imageData());
    if (!image.isNull()) return add_image(image, page, center, true);
  }
  if (mime != nullptr && mime->hasUrls()) {
    for (const QUrl& url : mime->urls()) {
      const QString path = url.toLocalFile();
      if (looks_like_image_file(path)) return add_image_file(path, page, center, true);
    }
  }
  show_transient(tr("There is no image on the clipboard"));
  return false;
}

void MainWindow::on_text_width_requested(int page, const QString& box, double left_x,
                                         double width) {
  if (!text_edits_allowed_ || text_font_ == nullptr) return;  // 재배치에 글꼴이 필요하다
  auto selected = view_->selected_text_box();
  if (!selected || selected->box != box.toStdString()) return;
  if (!warn_signature_once()) return;
  std::optional<double> origin_x;
  if (std::abs(left_x - selected->bbox.x) >= 0.5) origin_x = left_x;
  edit_->run(std::make_shared<edit::ReplaceTextBoxCommand>(page, box.toStdString(), text_font_,
                                                           std::nullopt, std::nullopt, std::nullopt,
                                                           width, origin_x));
}

void MainWindow::on_object_resize_requested(int page, const QString& id, onpdf::Rect before,
                                            onpdf::Rect after) {
  if (!content_edits_allowed_) return;
  if (has_foreign_object({id.toStdString()})) return explain_locked_object();
  if (!warn_signature_once()) return;
  edit_->run(std::make_shared<edit::ResizeObjectCommand>(page, id.toStdString(), before, after));
}

// ---- 기존 텍스트 줄 (#65) ----

QString MainWindow::line_reason_text(const engine::TextLineInfo& info) const {
  if (info.reason == "multi-line-object")
    return tr("This line shares an object with another line and cannot be edited separately");
  if (info.reason == "vertical") return tr("Vertical or rotated text cannot be edited");
  if (info.reason == "nested-form")
    return tr("Text in a form nested inside another form cannot be edited");
  return tr("This line cannot be edited");
}

void MainWindow::on_text_line_selected(bool has, int, const engine::TextLineInfo& info) {
  update_edit_actions();
  update_property_buttons();
  if (has) ensure_font_catalog();
  if (!has) {
    if (selection_status_shown_) show_transient(QString());
    return;
  }
  QString text;
  if (session_.has_document() && session_.handle().info().signature_count > 0) {
    text = tr("Text in a signed document cannot be edited");
  } else if (!info.editable) {
    text = line_reason_text(info);
  } else {
    QStringList parts;
    parts << tr("Line")
          << QStringLiteral("%1 %2pt")
                 .arg(QString::fromStdString(info.font_name))
                 .arg(info.font_size, 0, 'f', 1);
    if (info.mixed_fonts) parts << tr("mixed fonts");
    if (!info.font_embedded || info.font_type3) parts << tr("Edited with a substitute font");
    parts << QString::fromStdString(info.text).left(60);
    text = parts.join(QStringLiteral(" · "));
  }
  statusBar()->showMessage(text);
  selection_status_shown_ = true;
}

bool MainWindow::run_line_replace(int page, const std::string& line_id, std::optional<QString> text,
                                  std::optional<double> size, std::optional<engine::Color> color,
                                  std::shared_ptr<const edit::FontFace> fallback_face) {
  if (!text_edits_allowed_ || text_font_ == nullptr) return false;
  if (!warn_signature_once()) return false;
  std::optional<std::string> utf8;
  if (text) utf8 = text->toUtf8().toStdString();
  if (fallback_face == nullptr) fallback_face = text_font_;
  auto command = std::make_shared<edit::ReplaceTextLineCommand>(page, line_id, utf8, size, color,
                                                                std::move(fallback_face));
  pending_line_edit_ = command;
  if (!edit_->run(command)) {
    pending_line_edit_.reset();
    return false;
  }
  return true;
}

void MainWindow::on_text_line_edit_requested(int page, const engine::TextLineInfo& info) {
  if (!text_edits_allowed_ || text_font_ == nullptr) return;
  if (session_.handle().info().signature_count > 0) {
    show_transient(tr("Text in a signed document cannot be edited"));
    return;
  }
  if (!info.editable) {
    show_transient(line_reason_text(info));
    return;
  }
  for (;;) {
    AnnotationDialog dialog(tr("Edit Line"), QString::fromStdString(info.text), this);
    dialog.setObjectName(QStringLiteral("text_input"));
    if (dialog.exec() != QDialog::Accepted) return;
    const QString text = dialog.text()
                             .normalized(QString::NormalizationForm_C)
                             .trimmed();  // 한 줄: 줄바꿈은 공백으로
    // (AnnotationDialog 는 여러 줄 입력이지만 줄 수정은 한 줄이다)
    if (text.isEmpty() || text.toStdString() == info.text) return;
    auto face = fallback_font_for(info.font_name, text);  // 원본 글꼴이 못 그릴 때 쓸 후보 (#73)
    const std::u32string missing =
        face->missing(text.toUtf8().toStdString());  // 대체 글꼴에도 없는 글자는 거절
    if (missing.empty()) {
      run_line_replace(page, info.id, text, std::nullopt, std::nullopt, std::move(face));
      return;
    }
    QMessageBox warn(QMessageBox::Warning, tr("Edit Line"),
                     tr("Some characters are not in the font: %1\nPlease use other characters.")
                         .arg(QString::fromStdU32String(missing.substr(0, 10))),
                     QMessageBox::Ok, this);
    warn.setObjectName(QStringLiteral("text_missing_glyphs"));
    warn.exec();
  }
}

void MainWindow::on_text_line_delete_requested(int page, const engine::TextLineInfo& info) {
  if (!content_edits_allowed_) return;
  if (session_.handle().info().signature_count > 0) {
    show_transient(tr("Text in a signed document cannot be edited"));
    return;
  }
  if (!info.editable) {
    show_transient(line_reason_text(info));
    return;
  }
  edit_->run(std::make_shared<edit::RemoveTextLineCommand>(page, info.id));
}

void MainWindow::announce_line_edit() {
  if (edit_->busy()) return;
  auto command = pending_line_edit_.lock();
  if (!command) return;
  pending_line_edit_.reset();
  if (edit_->undo_command() != command) return;  // 실패했다 (on_edit_failed 가 알린다)
  auto* replace = dynamic_cast<edit::ReplaceTextLineCommand*>(command.get());
  if (replace == nullptr) return;
  if (!replace->box().empty()) {  // 고친 줄은 OnPdf 텍스트 상자가 됐다: 선택 도구로 돌아가 그
                                  // 상자를 고른다 (이미지 추가와 같다)
    tool_actions_[0]->setChecked(true);
    view_->set_tool(AnnotationTool::kSelect);
    view_->select_after_refresh(replace->page(), replace->box());
  }
  // 안내는 새 상자의 선택 문구 뒤에 보여야 남는다 (재선택이 상태 표시줄을 덮는다)
  pending_line_notice_ = replace->used_fallback()
                             ? tr("Some characters are not in the original font, so %1 was used")
                                   .arg(QString::fromStdString(replace->fallback_family()))
                             : QString();
  if (replace->box().empty() && !pending_line_notice_.isEmpty())
    show_transient(std::exchange(pending_line_notice_, QString()));
}

void MainWindow::explain_locked_object() {
  // 다른 프로그램이 만든 이미지를 옮기면 PDFium 이 그 쪽 내용 스트림을 통째로 다시 써 글자
  // 모양·색이 바뀔 수 있다 (#57)
  statusBar()->showMessage(tr("Images that were already in the document cannot be moved, resized "
                              "or deleted — the page content could be damaged"),
                           6000);
}

bool MainWindow::delete_selected_text_box() {
  if (auto line = view_->selected_line()) {  // 기존 줄 (#65)
    on_text_line_delete_requested(line->page, line->info);
    return true;
  }
  // 텍스트 상자·이미지 공용: 떼어 문서가 보관 (재생성이 아니라 빈 줄·옛 객체 문제가 없다, #49)
  auto selected = view_->selected_object();
  if (!selected || !content_edits_allowed_) return false;
  if (view_->selected_object_locked()) {
    explain_locked_object();
    return false;
  }
  if (!warn_signature_once()) return false;
  if (!edit_->run(std::make_shared<edit::RemoveObjectsCommand>(selected->page, selected->ids)))
    return false;
  view_->clear_text_box_selection();
  return true;
}

void MainWindow::on_text_edit_requested(int, const DocumentView::SelectedTextBox& box) {
  if (!text_edits_allowed_ || text_font_ == nullptr) return;
  for (;;) {
    AnnotationDialog dialog(tr("Edit Text"), QString::fromStdString(box.text), this);
    dialog.setObjectName(QStringLiteral("text_input"));
    if (dialog.exec() != QDialog::Accepted) return;
    const QString text = dialog.text().normalized(QString::NormalizationForm_C);
    if (text.trimmed().isEmpty() || text.toStdString() == box.text)
      return;                                            // 빈 내용·같은 내용은 무시 (삭제는 Delete)
    auto face = fallback_font_for(box.font_name, text);  // 같은 계열의 대체 글꼴 (#73)
    const std::u32string missing = face->missing(text.toUtf8().toStdString());
    if (missing.empty()) {
      if (box.original_font)
        run_line_replace(box.page, box.box, text, std::nullopt, std::nullopt,
                         face);  // 원본 글꼴로 고친 줄은 다시 원본 글꼴로 (#65)
      else
        replace_selected_text(text, std::nullopt, std::nullopt, face);
      return;
    }
    QMessageBox warn(QMessageBox::Warning, tr("Text"),
                     tr("Some characters are not in the font: %1\nPlease use other characters.")
                         .arg(QString::fromStdU32String(missing.substr(0, 10))),
                     QMessageBox::Ok, this);
    warn.setObjectName(QStringLiteral("text_missing_glyphs"));
    warn.exec();
  }
}

bool MainWindow::replace_selected_text(std::optional<QString> utf8, std::optional<double> size,
                                       std::optional<engine::Color> color,
                                       std::shared_ptr<const edit::FontFace> face) {
  auto selected = view_->selected_text_box();
  if (!selected || !text_edits_allowed_ || text_font_ == nullptr) return false;
  if (!warn_signature_once()) return false;
  std::optional<std::string> text;
  if (utf8) text = utf8->toUtf8().toStdString();
  if (color) color->a = 255;
  if (face == nullptr)
    face = fallback_font_for(selected->font_name,
                             QString::fromStdString(selected->text));  // 같은 글꼴 유지 (#73)
  return edit_->run(std::make_shared<edit::ReplaceTextBoxCommand>(
      selected->page, selected->box, std::move(face), text, size, color));
}

void MainWindow::on_ink_requested(int page, const std::vector<onpdf::Point>& points,
                                  onpdf::Rect bbox) {
  if (!annotation_edits_allowed_ || points.size() < 2) return;
  if (!warn_signature_once()) return;
  engine::AnnotationSpec spec;
  spec.kind = engine::AnnotationKind::kInk;
  spec.ink = {points};
  spec.rect = bbox;
  spec.border_width = shape_border_width_;
  spec.color = annotation_color_;
  spec.color.a = shape_alpha_;
  spec.flags = engine::kAnnotFlagPrint;
  spec.author = annotation_author();
  edit_->run(std::make_shared<edit::AddAnnotationsCommand>(
      std::vector<std::pair<int, engine::AnnotationSpec>>{{page, std::move(spec)}}));
}

void MainWindow::on_shape_requested(int page, engine::AnnotationKind kind, onpdf::Rect rect) {
  if (!annotation_edits_allowed_ || rect.empty()) return;
  if (!warn_signature_once()) return;
  engine::AnnotationSpec spec;
  spec.kind = kind;
  spec.rect = rect;
  spec.border_width = shape_border_width_;
  spec.color = annotation_color_;
  spec.color.a = shape_alpha_;
  spec.flags = engine::kAnnotFlagPrint;
  spec.author = annotation_author();
  edit_->run(std::make_shared<edit::AddAnnotationsCommand>(
      std::vector<std::pair<int, engine::AnnotationSpec>>{{page, std::move(spec)}}));
}

void MainWindow::on_annotation_selected(bool has, const engine::AnnotationInfo& info) {
  update_edit_actions();
  update_property_buttons();
  if (!has) {
    // 선택 정보만 지운다: "다시 만들었습니다" 같은 안내가 편집 완료 직후의 해제로 사라지지 않게
    // (리뷰 W1)
    if (selection_status_shown_) show_transient(QString());
    return;
  }
  QStringList parts{kind_name(info.kind)};
  // /BS 주석의 두께는 읽을 수 없어 표시하지 않는다
  if (has_border_width(info.kind) && !(info.has_bs && !info.has_border))
    parts << width_text(info.has_border ? info.border_width : 1.0);
  if (info.opacity < 0.999f) parts << opacity_text(info.opacity);
  if (!info.author.empty()) parts << QString::fromStdString(info.author);
  if (!info.modified.empty()) parts << short_date(info.modified);
  if (!info.contents.empty()) parts << QString::fromStdString(info.contents).left(60);
  statusBar()->showMessage(parts.join(QStringLiteral(" · ")));
  selection_status_shown_ = true;
}

bool MainWindow::delete_selected_annotation() {
  auto selected = view_->selected_annotation();
  if (!selected)
    return delete_selected_text_box();  // 툴바 "주석 삭제" 는 선택한 텍스트 상자에도 동작한다
  if (!delete_annotation_action_->isEnabled()) return false;
  if (!warn_signature_once()) return false;
  const int page = view_->selected_annotation_page();
  if (!edit_->run(std::make_shared<edit::RemoveAnnotationCommand>(page, selected->id)))
    return false;
  view_->clear_annotation_selection();  // 거절되면 선택을 남긴다
  return true;
}

void MainWindow::on_annotation_edit_requested(int page, const engine::AnnotationInfo& info) {
  if (!annotation_edits_allowed_ || info.kind != engine::AnnotationKind::kText) return;
  AnnotationDialog dialog(tr("Edit Note"), QString::fromStdString(info.contents), this);
  if (dialog.exec() != QDialog::Accepted) return;
  const QString text = dialog.text();
  if (text.toStdString() == info.contents) return;
  if (!warn_signature_once()) return;
  edit_->run(
      std::make_shared<edit::SetAnnotationContentsCommand>(page, info.id, text.toStdString()));
}

// ---------------------------------------------------------------- 병합·분할

namespace {

QString q_filename(const std::filesystem::path& p) {
  return QString::fromStdString(onpdf::path_to_utf8(p.filename()));
}

QString structure_error_text(const onpdf::Error& e) {
  switch (e.code) {
    case ErrorCode::kPasswordRequired:
      return MainWindow::tr("A document needs a password: %1")
          .arg(QString::fromStdString(e.message));
    case ErrorCode::kPermissionDenied:
      return MainWindow::tr("A document does not allow page extraction: %1")
          .arg(QString::fromStdString(e.message));
    case ErrorCode::kNotWritable:
      return MainWindow::tr("Cannot write the file. It is read-only or in use by another program.");
    case ErrorCode::kFolderNotWritable:
      return MainWindow::tr("Cannot create a new file in this folder. Choose another folder.");
    case ErrorCode::kSaveVerifyFailed:
      return MainWindow::tr("Could not verify the created file: %1")
          .arg(QString::fromStdString(e.message));
    default:
      return QString::fromStdString(e.message);
  }
}

}  // namespace

void MainWindow::show_structure_progress(const QString& title) {
  delete structure_progress_;
  structure_progress_ = new QProgressDialog(title, tr("Cancel"), 0, 100, this);
  structure_progress_->setObjectName(QStringLiteral("structure_progress"));
  structure_progress_->setWindowTitle(title);
  structure_progress_->setWindowModality(
      Qt::NonModal);  // 모달 진행 창은 setValue 마다 이벤트를 재진입 처리한다 (인쇄와 같은 이유)
  structure_progress_->setMinimumDuration(0);
  structure_progress_->setAutoClose(false);
  structure_progress_->setAutoReset(false);
  structure_progress_->setRange(0, 0);  // 입력을 읽는 동안은 진행률이 없다 → busy 표시 (리뷰 I9)
  structure_progress_->setLabelText(tr("%1 (preparing…)").arg(title));
  structure_progress_->show();
  QProgressDialog* progress = structure_progress_;
  connect(
      structure_job_, &StructureJob::progress, progress,
      [progress, title](int percent) {
        if (progress->maximum() == 0) {
          progress->setRange(0, 100);
          progress->setLabelText(title);
        }
        progress->setValue(percent);
      },
      Qt::QueuedConnection);
  connect(progress, &QProgressDialog::canceled, this, [this] {
    structure_job_->cancel();
    show_transient(tr("The result will be discarded when the job finishes…"));
  });
}

void MainWindow::on_merge_action() {
  if (!merge_action_->isEnabled()) return;
  const QString merge_dir =
      start_dir(DirKind::kMerge,
                session_.has_document() ? QFileInfo(session_.path()).absolutePath() : QString());
  const QStringList files =
      QFileDialog::getOpenFileNames(this, tr("PDFs to merge (combined in the order chosen)"),
                                    merge_dir, tr("PDF documents (*.pdf)"));
  if (files.isEmpty()) return;
  remember_dir(DirKind::kMerge, files.first());
  // 파일은 작업 스레드에서 읽는다 (큰 파일을 메인에서 열면 UI 가 멈춘다). 암호가 필요하면 결과로
  // 돌아와 묻고 다시 시도한다
  std::vector<engine::StructureInput> inputs;
  for (const QString& file : files)
    inputs.push_back({onpdf::path_from_utf8(file.toUtf8().toStdString()), ""});
  const QFileInfo first(files.first());
  const QString suggested =
      first.absolutePath() + QLatin1Char('/') + first.completeBaseName() + tr("-merged.pdf");
  const QString output = QFileDialog::getSaveFileName(this, tr("Save Merged PDF"), suggested,
                                                      tr("PDF documents (*.pdf)"));
  if (output.isEmpty()) return;
  merge_files(std::move(inputs), output);
}

bool MainWindow::merge_files(std::vector<engine::StructureInput> inputs, const QString& output) {
  if (inputs.empty() || output.isEmpty() || structure_job_->active()) return false;
  const auto out = onpdf::path_from_utf8(output.toUtf8().toStdString());
  for (const auto& in : inputs) {
    std::error_code ec;
    if (std::filesystem::exists(out, ec) && std::filesystem::equivalent(out, in.path, ec)) {
      show_transient(tr("The output file is the same as an input file"));
      return false;
    }
  }
  structure_kind_ = StructureKind::kMerge;
  structure_outputs_ = {out};
  structure_folder_ = QFileInfo(output).absolutePath();
  pending_merge_inputs_ = inputs;
  pending_merge_output_ = output;
  show_structure_progress(tr("Merging…"));
  const bool started =
      structure_job_->start([inputs = std::move(inputs),
                             out](const engine::ProgressFn& progress) -> StructureJob::Outcome {
        int expected = 0;
        for (const auto& in : inputs) {
          auto n = engine::QpdfTools::page_count(in.path, in.password);
          if (!n) return std::unexpected(n.error());
          expected += *n;
        }
        engine::SafeWriteHooks hooks;
        hooks.write = [&](const std::vector<std::filesystem::path>& temps) -> onpdf::Result<void> {
          auto r = engine::QpdfTools::merge(inputs, temps.front(), progress);
          if (!r) return std::unexpected(r.error());
          return {};
        };
        hooks.verify = [&](std::size_t, const std::filesystem::path&,
                           const std::filesystem::path& temp) -> onpdf::Result<void> {
          if (auto c = engine::QpdfTools::check(temp); !c) return std::unexpected(c.error());
          auto n = engine::QpdfTools::page_count(temp);
          if (!n) return std::unexpected(n.error());
          if (*n != expected)
            return onpdf::make_error(ErrorCode::kSaveVerifyFailed, "merged page count mismatch");
          return {};
        };
        return engine::write_files_safely({out}, hooks);
      });
  update_edit_actions();
  return started;
}

void MainWindow::on_split_action() {
  if (!split_action_->isEnabled() || !session_.has_document()) return;
  // 분할은 디스크의 파일을 읽는다: 수정됐으면 먼저 저장할지 묻는다 (저장 안 함이면 디스크 상태로
  // 나눈다)
  if (edit_->is_modified()) {
    maybe_save([this] { prompt_split(); });
    return;
  }
  prompt_split();
}

void MainWindow::prompt_split() {
  if (!session_.has_document()) return;
  const int count = session_.handle().info().page_count;
  QInputDialog dialog(this);
  dialog.setObjectName(QStringLiteral("split_every"));
  dialog.setWindowTitle(tr("Split"));
  dialog.setLabelText(tr("Split every how many pages? (%1 pages in total)").arg(count));
  dialog.setInputMode(QInputDialog::IntInput);
  dialog.setIntRange(1, std::max(1, count));
  dialog.setIntValue(1);
  if (dialog.exec() != QDialog::Accepted) return;
  const QString dir = QFileDialog::getExistingDirectory(
      this, tr("Folder for the split files"),
      start_dir(DirKind::kSplit, QFileInfo(session_.path()).absolutePath()));
  if (dir.isEmpty()) return;
  remember_dir(DirKind::kSplit, dir);
  split_document(dialog.intValue(), dir);
}

bool MainWindow::split_document(int every, const QString& directory) {
  if (!session_.has_document() || every <= 0 || directory.isEmpty() || structure_job_->active())
    return false;
  const int count = session_.handle().info().page_count;
  const auto ranges = onpdf::ranges_every(every, count);
  if (ranges.empty()) return false;
  const engine::StructureInput input{
      onpdf::path_from_utf8(session_.path().toUtf8().toStdString()),
      session_.password().toStdString()};  // 암호 문서도 나눌 수 있게 (리뷰 W2)
  const std::string stem = QFileInfo(session_.path()).completeBaseName().toUtf8().toStdString();
  const auto dir = onpdf::path_from_utf8(directory.toUtf8().toStdString());
  std::vector<std::filesystem::path> outs;
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    outs.push_back(dir / onpdf::path_from_utf8(onpdf::output_name(
                             stem + "-%d.pdf", static_cast<int>(i) + 1,
                             static_cast<int>(ranges.size()), ranges[i], count)));
  }
  int existing = 0;
  for (const auto& p : outs) {
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) ++existing;
  }
  if (existing > 0) {  // 폴더 선택 창은 덮어쓰기를 묻지 않으므로 여기서 한 번 묻는다 (리뷰 W3)
    QMessageBox box(QMessageBox::Question, tr("Split"),
                    tr("%n file(s) with the same name will be overwritten. Continue?", nullptr,
                       static_cast<int>(existing)),
                    QMessageBox::NoButton, this);
    box.setObjectName(QStringLiteral("split_overwrite"));
    QPushButton* go = box.addButton(tr("Overwrite"), QMessageBox::AcceptRole);
    QPushButton* cancel = box.addButton(tr("Cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(cancel);
    box.exec();
    if (box.clickedButton() != go) return false;
  }
  structure_kind_ = StructureKind::kSplit;
  structure_outputs_ = outs;
  structure_folder_ = directory;
  show_structure_progress(tr("Splitting…"));
  const bool started = structure_job_->start(
      [input, ranges, outs](const engine::ProgressFn& progress) -> StructureJob::Outcome {
        engine::SafeWriteHooks hooks;
        hooks.write = [&](const std::vector<std::filesystem::path>& temps) -> onpdf::Result<void> {
          for (std::size_t i = 0; i < temps.size(); ++i) {
            if (auto r = engine::QpdfTools::extract(input, ranges[i], temps[i]); !r)
              return std::unexpected(r.error());
            progress(static_cast<int>((i + 1) * 100 / temps.size()));
          }
          return {};
        };
        hooks.verify = [&](std::size_t i, const std::filesystem::path&,
                           const std::filesystem::path& temp) -> onpdf::Result<void> {
          auto n = engine::QpdfTools::page_count(temp);
          if (!n) return std::unexpected(n.error());
          if (*n != ranges[i].last - ranges[i].first + 1)
            return onpdf::make_error(ErrorCode::kSaveVerifyFailed, "split page count mismatch");
          return {};
        };
        return engine::write_files_safely(outs, hooks);
      });
  update_edit_actions();
  return started;
}

void MainWindow::on_structure_finished(bool cancelled) {
  const StructureJob::Outcome& outcome = structure_job_->outcome();
  if (structure_progress_ != nullptr) {
    structure_progress_->hide();
    structure_progress_->deleteLater();
    structure_progress_ = nullptr;
  }
  const StructureKind kind = structure_kind_;
  const std::vector<std::filesystem::path> outputs = std::move(structure_outputs_);
  structure_kind_ = StructureKind::kNone;
  update_edit_actions();
  if (cancelled) {
    merge_password_attempts_ = 0;
    // qpdf 작업은 중단할 수 없어 끝까지 돈다. 결과 파일은 지우지 않는다 — 사용자가 덮어쓰기로 고른
    // 기존 파일이나 이전 분할 결과를 지워 버릴 수 있기 때문 (리뷰 C2). 성공했으면 알리기만,
    // 실패했으면 원래 아무것도 없다
    show_transient(
        outcome ? tr("The job finished before it could be cancelled, so the files were created")
                : tr("Cancelled"));
    return;
  }
  if (!outcome && kind == StructureKind::kMerge &&
      outcome.error().code == ErrorCode::kPasswordRequired && merge_password_attempts_ < 5) {
    // 어느 입력인지는 메시지 앞의 경로로 찾는다 ("<파일>: ..."). 묻고 같은 병합을 다시 시도한다
    // (리뷰 I4·W1)
    const std::string& message = outcome.error().message;
    for (auto& input : pending_merge_inputs_) {
      const std::string path = onpdf::path_to_utf8(input.path);
      if (message.rfind(path, 0) != 0) continue;
      QInputDialog dialog(this);
      dialog.setObjectName(QStringLiteral("merge_password"));
      dialog.setWindowTitle(tr("Password"));
      dialog.setLabelText(
          tr("%1 is password protected. Enter the password.").arg(q_filename(input.path)));
      dialog.setTextEchoMode(QLineEdit::Password);
      if (dialog.exec() != QDialog::Accepted || dialog.textValue().isEmpty()) {
        show_transient(tr("Merge cancelled"));
        return;
      }
      input.password = dialog.textValue().toStdString();
      ++merge_password_attempts_;
      merge_files(pending_merge_inputs_, pending_merge_output_);
      return;
    }
  }
  merge_password_attempts_ = 0;
  if (!outcome) {
    QMessageBox box(QMessageBox::Warning,
                    kind == StructureKind::kMerge ? tr("Could not merge") : tr("Could not split"),
                    tr("%1\n\nNo files were created.").arg(structure_error_text(outcome.error())),
                    QMessageBox::Ok, this);
    box.setObjectName(QStringLiteral("structure_failed"));
    box.exec();
    return;
  }
  if (kind == StructureKind::kMerge) {
    show_transient(tr("Merged"));
    open_file(QString::fromStdString(onpdf::path_to_utf8(outputs.front())));
    return;
  }
  QMessageBox box(QMessageBox::Information, tr("Split"),
                  tr("Saved %n file(s).", nullptr, static_cast<int>(outcome->size())),
                  QMessageBox::NoButton, this);
  box.setObjectName(QStringLiteral("split_done"));
  QPushButton* open_folder = box.addButton(tr("Open Folder"), QMessageBox::ActionRole);
  box.addButton(tr("OK"), QMessageBox::AcceptRole);
  box.exec();
  if (box.clickedButton() == open_folder)
    QDesktopServices::openUrl(QUrl::fromLocalFile(structure_folder_));
}

void MainWindow::on_thumbnail_context_menu(const QPoint& global_pos,
                                           const std::vector<int>& pages) {
  if (pages.empty() || !view_->has_document()) return;
  const bool enabled = delete_pages_action_->isEnabled();
  QMenu menu(this);
  menu.setObjectName(QStringLiteral("thumbnail_menu"));
  QAction* del = menu.addAction(tr("Delete Pages"), this, [this, pages] { delete_pages(pages); });
  QAction* cw = menu.addAction(tr("Rotate Right"), this, [this, pages] { rotate_pages(pages, 1); });
  QAction* ccw = menu.addAction(tr("Rotate Left"), this, [this, pages] { rotate_pages(pages, 3); });
  menu.addSeparator();
  const int before = pages.front();
  const int after = pages.back() + 1;
  QAction* ins_before =
      menu.addAction(tr("Insert Pages from File (Before)..."), this, [this, before] {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("PDF to Insert Pages From"),
            start_dir(DirKind::kInsert, QFileInfo(session_.path()).absolutePath()),
            tr("PDF documents (*.pdf)"));
        if (path.isEmpty()) return;
        remember_dir(DirKind::kInsert, path);
        insert_pages_from(path, before);
      });
  QAction* ins_after = menu.addAction(tr("Insert Pages from File (After)..."), this, [this, after] {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("PDF to Insert Pages From"),
        start_dir(DirKind::kInsert, QFileInfo(session_.path()).absolutePath()),
        tr("PDF documents (*.pdf)"));
    if (path.isEmpty()) return;
    remember_dir(DirKind::kInsert, path);
    insert_pages_from(path, after);
  });
  for (QAction* action : {del, cw, ccw, ins_before, ins_after}) action->setEnabled(enabled);
  menu.exec(global_pos);
}

void MainWindow::on_document_changed(const onpdf::engine::ChangeSet& change) {
  pending_insert_.reset();
  if (change.structural) {
    // 쪽 수·순서가 바뀌었다: 뷰·썸네일을 새 쪽 목록으로 다시 세우고(현재 쪽은 새 범위로 클램프), 쪽
    // 번호·목차를 갱신한다
    const int n = session_.handle().info().page_count;
    page_spin_->setRange(
        1, std::max(1, n));  // 뷰 재구성이 current_page_changed 로 값을 넣기 전에 범위를 넓힌다
    page_total_label_->setText(tr("/ %1").arg(n));
    view_->rebuild_pages(change.focus_page >= 0 ? change.focus_page
                                                : std::max(0, view_->current_page()));
    thumbnail_panel_->reset_pages(std::max(0, view_->current_page()));
    thumbnail_panel_->select_pages(
        change.selected_pages);    // 옮긴·넣은·되살린 쪽을 다시 선택해 연속 조작이 이어진다
    print_prefs_applied_ = false;  // 인쇄 기본값의 쪽 범위는 이전 쪽 수 기준이었다
    load_outline(false);           // 목차 항목의 쪽 번호가 밀렸다 (탭은 바꾸지 않는다)
  } else {
    view_->invalidate_pages(change.pages, !change.new_sizes.empty());
    thumbnail_panel_->invalidate_pages(change.pages);
  }
  // 검색 결과 사각형은 이전 모양 기준 → 같은 검색어로 다시 찾는다
  if (search_ && search_->active() && !search_bar_->query().trimmed().isEmpty()) {
    reset_search();
    on_search_query(search_bar_->query(), search_bar_->options());
  }
  if (last_image_command_ && !last_image_command_->object_id().empty() && pending_image_) {
    // 방금 넣은 이미지: 선택 도구로 돌아가 그 이미지를 고른다 (목록이 새로 오면)
    const int page = pending_image_->page;
    const std::string id = last_image_command_->object_id();
    pending_image_.reset();
    last_image_command_.reset();
    tool_actions_[0]->setChecked(true);
    view_->set_tool(AnnotationTool::kSelect);
    view_->select_after_refresh(page, id);
  }
  if (change.spacing_lost) {
    // 편집은 됐지만 PDFium 재생성으로 사라진 자간을 다시 넣지 못했다 (#87)
    statusBar()->showMessage(
        tr("Letter spacing could not be restored; the edited text uses default spacing"), 6000);
  }
  refresh_status();
}

void MainWindow::on_save() {
  if (!save_action_->isEnabled()) return;
  start_save(session_.path(), [] {});
}

void MainWindow::on_save_as() {
  if (!save_as_action_->isEnabled()) return;
  const QString path = QFileDialog::getSaveFileName(this, tr("Save As"), session_.path(),
                                                    tr("PDF documents (*.pdf)"));
  if (!path.isEmpty()) start_save(path, [] {});
}

bool MainWindow::save_to(const QString& path) {
  return start_save(path, [] {});
}

bool MainWindow::start_save(const QString& path, std::function<void()> then) {
  if (!session_.has_document() || path.isEmpty()) return false;
  set_action(QStringLiteral("save"));  // (#131)
  settle_form_edits();                 // 입력 중인 칸까지 저장한다 (#51)
  if (saving_ || edit_->busy()) {      // 몇 ms 의 창이지만 조용히 멈추지 않는다 (리뷰 I1)
    show_transient(tr("Cannot save right now. Try again in a moment"));
    return false;
  }
  saving_ = true;
  update_edit_actions();
  show_transient(tr("Saving…"));
  const QByteArray utf8 = path.toUtf8();
  UiDispatcher* dispatcher = &dispatcher_;
  QPointer<MainWindow> self(this);
  const int generation = document_generation_;  // 문서를 열 때마다 증가
  session_.handle().save(
      onpdf::path_from_utf8(std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()))),
      {},
      // 워커 스레드: 메인으로 넘기기만
      [dispatcher, self, path, generation,
       then = std::move(then)](onpdf::Result<engine::SaveResult> result) {
        auto shared = std::make_shared<onpdf::Result<engine::SaveResult>>(std::move(result));
        dispatcher->post_to(self, [path, generation, then, shared](MainWindow* window) {
          window->saving_ = false;
          QTimer::singleShot(0, window, [window] {
            window->flush_form_commands();
          });                                                      // 저장 중 쌓인 폼 입력 (#51)
          if (generation != window->document_generation_) return;  // 다른 문서로 바뀌었다
          if (!*shared) {
            window->update_edit_actions();
            QString reason = QString::fromStdString(shared->error().message);
            if (shared->error().code == ErrorCode::kNotWritable) {
              reason = MainWindow::tr(
                  "Cannot write the file. It is read-only or in use by another program.\nSave it "
                  "under another name.");
            } else if (shared->error().code == ErrorCode::kFolderNotWritable) {
              reason = MainWindow::tr(
                  "Cannot create a new file in this folder.\nSave it under another name in another "
                  "folder.");
            }
            QMessageBox box(QMessageBox::Warning, MainWindow::tr("Could Not Save"),
                            MainWindow::tr("%1\n\nThe original file was not changed.").arg(reason),
                            QMessageBox::Ok, window);
            box.setObjectName(QStringLiteral("save_failed"));
            box.exec();
            return;
          }
          window->session_.set_path(path);
          window->edit_->mark_saved();          // state_changed → 제목·동작 갱신
          window->settings_->add_recent(path);  // (#129)
          window->remember_dir(DirKind::kSave, path);
          window->show_transient(
              (*shared)->incremental
                  ? MainWindow::tr("Saved (incrementally, to keep the signature)")
                  : MainWindow::tr("Saved"));
          then();
        });
      });
  return true;
}

void MainWindow::maybe_save(std::function<void()> then) {
  settle_form_edits();  // 입력 중인 칸도 "저장하지 않은 변경" 에 들어간다 (#51)
  if (!session_.has_document() || !edit_->is_modified()) {
    then();
    return;
  }
  QMessageBox box(QMessageBox::Warning, tr("Unsaved Changes"),
                  tr("Save changes to %1?").arg(QFileInfo(session_.path()).fileName()),
                  QMessageBox::NoButton, this);
  box.setObjectName(QStringLiteral("save_changes_question"));
  box.setInformativeText(tr("Your changes will be lost if you don't save them."));
  QPushButton* save = box.addButton(tr("Save"), QMessageBox::AcceptRole);
  box.addButton(tr("Don't Save"), QMessageBox::DestructiveRole);
  QPushButton* cancel = box.addButton(tr("Cancel"), QMessageBox::RejectRole);
  box.setDefaultButton(save);
  box.setEscapeButton(cancel);
  box.exec();
  if (box.clickedButton() == cancel || box.clickedButton() == nullptr) return;
  if (box.clickedButton() == save) {
    start_save(session_.path(), std::move(then));
    return;
  }
  then();  // 저장 안 함
}

void MainWindow::closeEvent(QCloseEvent* event) {
  if (worker_stalled()) {  // 워커가 멈춰 있으면 저장 확인도 종료(join)도 불가 — 강제 종료만 (#121)
    show_stall_warning();
    event->ignore();
    return;
  }
  if (structure_job_ != nullptr &&
      structure_job_->active()) {  // 병합·분할이 끝나기 전에는 닫지 않는다 (임시 파일 정리를 위해)
    show_transient(tr("You can close after merging or splitting finishes"));
    event->ignore();
    return;
  }
  if (allow_close_ || !session_.has_document() || !edit_->is_modified()) {
    if (saving_) {  // 저장이 끝날 때까지 닫지 않는다 (원본 교체 중)
      event->ignore();
      return;
    }
    save_window_state();                // 실제로 닫히는 유일한 경로 (#129)
    settings_->clear_session_marker();  // 정상 종료 (#131)
    event->accept();
    return;
  }
  event->ignore();  // 묻고, 저장이 끝난 뒤 다시 닫는다
  if (saving_) return;
  maybe_save([this] {
    allow_close_ = true;
    QTimer::singleShot(0, this, &QWidget::close);
  });
}

// ---------------------------------------------------------------- 인쇄

void MainWindow::update_print_action() {
  const bool has_pages = session_.has_document() && session_.handle().valid() &&
                         session_.handle().info().page_count > 0;
  const bool allowed = has_pages && session_.handle().info().print.allowed;
  const bool busy = print_job_ && print_job_->active();
  // 금지 문서도 켜 둔다: Cmd+P 가 권한 암호 해제 안내로 이어진다 (#23)
  print_action_->setEnabled(has_pages && !busy);
  const QString tip = has_pages && !allowed ? tr("This document does not allow printing. You can "
                                                 "unlock it with its permissions password")
                                            : tr("Print the document");
  print_action_->setStatusTip(tip);
  print_action_->setToolTip(tip);
  update_edit_actions();  // 인쇄 중에는 편집을 막는다
}

void MainWindow::on_print() {
  if (!print_action_->isEnabled() || !session_.has_document()) return;
  if (!session_.handle().info().print.allowed) {  // 권한 암호로 해제할 수 있다 (#23)
    offer_print_unlock();
    return;
  }
  if (!printer_) printer_ = std::make_unique<QPrinter>(QPrinter::HighResolution);
  const auto& info = session_.handle().info();
  if (!print_prefs_applied_) {
    apply_print_preferences(*printer_, info.print, info.page_count);
    print_prefs_applied_ = true;
  }
  const int current = std::max(0, view_->current_page());
  const auto run_dialog = [&](QWidget* parent) {
    QPrintDialog dialog(printer_.get(), parent);
    dialog.setWindowTitle(tr("Print"));
    dialog.setOptions(QAbstractPrintDialog::PrintPageRange |
                      QAbstractPrintDialog::PrintCurrentPage |
                      QAbstractPrintDialog::PrintCollateCopies | QAbstractPrintDialog::PrintToFile |
                      QAbstractPrintDialog::PrintShowPageSize);
    dialog.setMinMax(1, session_.handle().info().page_count);
    return dialog.exec() == QDialog::Accepted;
  };
  if (!run_dialog(this)) return;
  // 미리보기(#23): 용지 배치를 보고 배율·흑백을 고른다. 범위·용지를 바꾸려면 "설정…" 으로
  // 대화상자를 다시 연다 (선택한 배율·흑백은 유지)
  PrintPreviewDialog preview(session_.handle(), dispatcher_, *printer_,
                             make_print_request(*printer_, session_.handle().info(), current),
                             this);
  connect(&preview, &PrintPreviewDialog::settings_requested, &preview, [&] {
    if (!run_dialog(&preview)) return;
    PrintJob::Request again = make_print_request(*printer_, session_.handle().info(), current);
    again.scaling = preview.request().scaling;
    again.grayscale = preview.request().grayscale;
    preview.set_request(std::move(again));
  });
  if (preview.exec() != QDialog::Accepted) return;
  print_with(*printer_, preview.request());
}

void MainWindow::offer_print_unlock() {
  QMessageBox box(QMessageBox::Question, tr("Printing Not Allowed"),
                  tr("This document does not allow printing.\nIf you know its permissions (owner) "
                     "password, you can unlock printing."),
                  QMessageBox::NoButton, this);
  box.setObjectName(QStringLiteral("print_unlock_question"));
  QPushButton* enter = box.addButton(tr("Enter Permissions Password…"), QMessageBox::AcceptRole);
  QPushButton* cancel = box.addButton(tr("Cancel"), QMessageBox::RejectRole);
  box.setDefaultButton(enter);
  box.setEscapeButton(cancel);
  box.exec();
  if (box.clickedButton() != enter) return;
  // 다시 열면 편집 기록이 사라진다 → 분할과 같은 규칙으로 먼저 저장 여부를 묻는다
  maybe_save([this] { prompt_print_unlock_password(QString()); });
}

void MainWindow::prompt_print_unlock_password(const QString& note) {
  if (!session_.has_document()) return;
  QInputDialog dialog(this);
  dialog.setObjectName(QStringLiteral("print_unlock_password"));
  dialog.setWindowTitle(tr("Permissions Password"));
  dialog.setLabelText(
      (note.isEmpty() ? QString() : note + QLatin1Char('\n')) +
      tr("Permissions password for %1:").arg(QFileInfo(session_.path()).fileName()));
  dialog.setTextEchoMode(QLineEdit::Password);
  if (dialog.exec() != QDialog::Accepted) return;
  // 같은 파일을 그 암호로 다시 연다. 소유자 암호면 PDFium 이 권한 비트를 전부 켠다. 틀리면 세션이
  // 기존 문서를 유지한 채 password_required 를, 사용자 암호면 opened 를 보낸다
  print_unlock_ = PendingPrintUnlock{std::max(0, view_->current_page())};
  session_.open(session_.path(), dialog.textValue());
}

bool MainWindow::print_with(QPrinter& printer) {
  if (!session_.has_document() || !session_.handle().valid()) return false;
  return print_with(printer, make_print_request(printer, session_.handle().info(),
                                                std::max(0, view_->current_page())));
}

bool MainWindow::print_with(QPrinter& printer, PrintJob::Request request) {
  if (!session_.has_document() || !session_.handle().valid()) return false;
  if (print_job_ && print_job_->active()) return false;
  settle_form_edits();  // 입력 중인 칸 값으로 인쇄한다 (#51)
  if (edit_->busy()) {  // 편집 결과(쪽 크기)가 반영되기 전의 배치로 인쇄하지 않는다 (리뷰 I6)
    show_transient(tr("Applying edits. Try printing again in a moment"));
    return false;
  }
  const engine::DocumentHandle handle = session_.handle();
  if (!handle.info().print.allowed) {
    show_transient(tr("This document does not allow printing"));
    return false;
  }
  if (request.steps.empty()) {
    show_transient(tr("There are no pages to print"));
    return false;
  }
  print_job_ = std::make_unique<PrintJob>(handle, dispatcher_, printer, std::move(request));
  PrintJob* job = print_job_.get();

  delete print_progress_;
  print_progress_ =
      new QProgressDialog(tr("Printing…"), tr("Cancel"), 0, job->total_sheets(), this);
  print_progress_->setWindowTitle(tr("Print"));
  // 비모달: 모달 진행 대화상자는 setValue 마다 이벤트를 안에서 처리해 인쇄 처리에 재진입을
  // 일으키고, macOS 에서는 시트 애니메이션이 메인 스레드를 수백 ms 붙잡는다. 문서 교체·창 닫기는
  // 인쇄를 취소한다
  print_progress_->setWindowModality(Qt::NonModal);
  print_progress_->setMinimumDuration(300);
  print_progress_->setAutoClose(false);
  print_progress_->setAutoReset(false);
  auto* bar = new QProgressBar(print_progress_);
  bar->setFormat(tr("%v / %m sheets"));  // 레이블을 매 장 바꾸면 대화상자 크기를 다시 계산한다
  bar->setTextVisible(true);
  print_progress_->setBar(bar);
  QProgressDialog* progress = print_progress_;
  // 다음 이벤트 턴에 갱신: 그리기 처리 시간에 UI 갱신이 섞이지 않게
  connect(
      job, &PrintJob::progress, progress, [progress](int done, int) { progress->setValue(done); },
      Qt::QueuedConnection);
  connect(progress, &QProgressDialog::canceled, this, &MainWindow::cancel_print);
  connect(job, &PrintJob::finished, this, &MainWindow::on_print_finished);
  const bool started = job->start();
  update_print_action();  // 시작한 뒤에야 active 라 인쇄 중 비활성이 반영된다
  return started;
}

bool MainWindow::confirm_cancel_print_for(const QString& path) {
  QMessageBox box(QMessageBox::Question, tr("Printing"),
                  tr("A document is being printed. Cancel printing and open %1?")
                      .arg(QFileInfo(path).fileName()),
                  QMessageBox::NoButton, this);
  box.setObjectName(QStringLiteral("print_busy_question"));
  QPushButton* cancel_and_open =
      box.addButton(tr("Cancel Printing and Open"), QMessageBox::AcceptRole);
  QPushButton* keep = box.addButton(tr("Keep Printing"), QMessageBox::RejectRole);
  box.setDefaultButton(keep);  // 실수로 Enter 를 눌러도 인쇄가 끊기지 않게
  box.exec();
  if (box.clickedButton() != cancel_and_open) return false;
  cancel_print();  // finished(kCancelled) → "인쇄를 취소했습니다"
  return true;
}

void MainWindow::discard_print_job() {
  if (!print_job_) return;
  const bool was_active = print_job_->active();
  print_job_.reset();  // 소멸자가 신호 없이 취소 → on_print_finished 가 오지 않으므로 진행 창은
                       // 여기서 닫는다
  if (print_progress_ != nullptr) {
    print_progress_->hide();
    print_progress_->deleteLater();
    print_progress_ = nullptr;
  }
  if (was_active) show_transient(tr("Printing was cancelled to open a new document"));
}

void MainWindow::cancel_print() {
  if (print_job_ && print_job_->active()) print_job_->cancel();
}

void MainWindow::on_print_finished(PrintJob::Outcome outcome) {
  if (print_progress_ != nullptr) {
    print_progress_->hide();
    print_progress_->deleteLater();
    print_progress_ = nullptr;
  }
  update_print_action();
  const PrintJob* job = print_job_.get();
  switch (outcome) {
    case PrintJob::Outcome::kCompleted: {
      show_transient(tr("Printed %n sheet(s)", nullptr,
                        static_cast<int>(job != nullptr ? job->printed_sheets() : 0)));
      if (job != nullptr && !job->failed_pages().empty()) {
        QStringList pages;
        for (int page : job->failed_pages()) pages << QString::number(page + 1);
        QMessageBox::warning(
            this, tr("Some pages could not be printed"),
            tr("These pages were printed blank: %1").arg(pages.join(QStringLiteral(", "))));
      }
      break;
    }
    case PrintJob::Outcome::kCancelled:
      show_transient(tr("Printing cancelled"));
      break;
    case PrintJob::Outcome::kFailed:
      QMessageBox::warning(
          this, tr("Could not print"),
          tr("The printer cannot be opened or the document can no longer be read."));
      break;
  }
}

// ---------------------------------------------------------------- 사이드바

void MainWindow::load_outline(bool switch_tab) {
  outline_task_.cancel();  // 이전 문서의 목차 순회가 아직 대기 중이면 워커를 점유하지 않게
  const int generation = ++outline_generation_;
  outline_panel_->set_outline(nullptr);
  if (switch_tab) sidebar_->setCurrentWidget(thumbnail_panel_);
  UiDispatcher* dispatcher = &dispatcher_;
  QPointer<MainWindow> self(this);
  outline_task_ = session_.handle().load_outline(
      engine::Priority::kNormal,
      // 워커 스레드: 메인으로 넘기기만
      [dispatcher, self, generation, switch_tab](onpdf::Result<view::Outline> result) {
        auto shared = std::make_shared<onpdf::Result<view::Outline>>(std::move(result));
        dispatcher->post_to(self, [generation, shared, switch_tab](MainWindow* window) {
          if (generation != window->outline_generation_) return;  // 그 사이 다른 문서를 열었다
          std::shared_ptr<const view::Outline> outline;
          if (*shared) {
            outline = std::make_shared<const view::Outline>(std::move(**shared));
          } else if (shared->error().code != ErrorCode::kCancelled) {
            window->show_transient(MainWindow::tr("Could not read the outline"));
          }
          window->outline_panel_->set_outline(outline);
          // 문서를 열 때: 목차가 있으면 목차 탭, 없으면 썸네일 탭. 편집 뒤 재로드는 탭을 그대로
          // 둔다
          if (switch_tab) {
            window->sidebar_->setCurrentWidget(
                outline && !outline->empty() ? static_cast<QWidget*>(window->outline_panel_)
                                             : static_cast<QWidget*>(window->thumbnail_panel_));
          }
          window->update_outline_highlight();
        });
      });
}

void MainWindow::on_outline_item(const onpdf::view::Link& target) {
  if (!view_->has_document()) return;
  if (target.kind == view::LinkKind::kUri) {
    on_link_activated(target);  // is_safe_uri + 사용자 확인
    return;
  }
  navigate_to(target);
}

void MainWindow::navigate_to(const onpdf::view::Link& target) {
  if (!view_->has_document() || target.kind != view::LinkKind::kPage) return;
  if (target.page < 0 || target.page >= view_->page_count()) return;
  if (target.location) {  // 링크: 추출 때 변환된 쪽 포인트 → 바로 이동
    view_->scroll_to_page_point(target.page, *target.location);
    return;
  }
  if (!target.user_location) {
    view_->go_to_page(target.page);
    return;
  }
  // 목차: /XYZ 위치는 쪽을 열어야 쪽 포인트로 바꿀 수 있다 → 워커에서 변환 후 이동
  const int generation = document_generation_;
  const int page = target.page;
  UiDispatcher* dispatcher = &dispatcher_;
  QPointer<MainWindow> self(this);
  session_.handle().resolve_location(
      page, *target.user_location, engine::Priority::kHigh,
      [dispatcher, self, generation, page](onpdf::Result<onpdf::Point> result) {
        auto shared = std::make_shared<onpdf::Result<onpdf::Point>>(std::move(result));
        dispatcher->post_to(self, [generation, page, shared](MainWindow* window) {
          // 그 사이 다른 문서를 열었으면 버린다 (document_generation_ 은 문서를 열 때마다 증가)
          if (generation != window->document_generation_ || !window->view_->has_document()) return;
          if (*shared) {
            window->view_->scroll_to_page_point(page, **shared);
          } else {
            window->view_->go_to_page(page);
          }
        });
      });
}

void MainWindow::update_outline_highlight() {
  if (!view_->has_document() || !sidebar_->isVisible() ||
      sidebar_->currentWidget() != outline_panel_)
    return;
  if (auto pos = view_->reading_position())
    outline_panel_->highlight_for(pos->first, pos->second.y);
}

// ---------------------------------------------------------------- 검색

void MainWindow::reset_search() {
  view_->set_search(nullptr);
  search_.reset();  // 진행 중 작업 취소, 늦은 콜백은 세션 수명 확인으로 무시
  if (!session_.has_document()) {
    update_search_status();
    return;
  }
  engine::DocumentHandle handle = session_.handle();
  UiDispatcher* dispatcher = &dispatcher_;
  QPointer<MainWindow> self(this);
  search_ = std::make_unique<view::SearchSession>(
      [handle, dispatcher, self](int page, const std::string& query, view::SearchOptions options,
                                 view::SearchSession::PageCallback on_done) {
        // kLow: 같은 우선순위 FIFO 라 텍스트 인덱스·프리페치 타일과 번갈아 처리된다 (굶기지 않음)
        return handle.search_page(
            page, query, options, engine::Priority::kLow,
            // 워커 스레드: 메인으로 넘기기만
            [dispatcher, self,
             on_done = std::move(on_done)](onpdf::Result<view::PageSearchResult> result) {
              auto shared =
                  std::make_shared<onpdf::Result<view::PageSearchResult>>(std::move(result));
              dispatcher->post_to(self,
                                  [on_done, shared](MainWindow*) { on_done(std::move(*shared)); });
            });
      });
  search_->on_changed = [this] {
    update_search_status();
    view_->viewport()->update();
  };
  search_->on_current_changed = [this] {
    if (auto hit = search_->current()) {
      // 자동 선택된 첫 결과는 사용자가 검색 시작 후 스크롤하지 않았을 때만 따라간다
      const bool user_moved = view_->verticalScrollBar()->value() != search_scroll_anchor_;
      if (!search_->current_is_automatic() || !user_moved) {
        view_->reveal(hit->page, search_->current_rects());
        search_scroll_anchor_ = view_->verticalScrollBar()->value();
      }
    }
    update_search_status();
    view_->viewport()->update();
  };
  view_->set_search(search_.get());
  update_search_status();
}

void MainWindow::open_find() {
  QString initial;
  // 선택이 한 쪽 안이고 그 쪽 인덱스가 이미 있을 때만 (미로드 쪽을 동기 로드하지 않도록)
  const auto& sel = view_->selection();
  if (view_->has_selection() && sel.start().page == sel.end().page &&
      view_->text_index(sel.start().page) != nullptr) {
    const QString selected = view_->selected_text();
    if (!selected.contains(QLatin1Char('\n')) && selected.size() <= 100)
      initial = selected.trimmed();
  }
  search_bar_->open_with(initial);
  // 다시 열었는데 입력이 그대로라 textChanged 가 오지 않는 경우: 비활성 세션이면 바로 검색
  if (search_ && !search_->active() && !search_bar_->query().trimmed().isEmpty()) {
    on_search_query(search_bar_->query(), search_bar_->options());
  }
}

void MainWindow::on_search_query(const QString& query, view::SearchOptions options) {
  if (!search_) return;
  if (query.trimmed().isEmpty()) {
    search_->clear();
    return;
  }
  const QByteArray utf8 = query.toUtf8();
  search_scroll_anchor_ = view_->verticalScrollBar()->value();
  search_->start(std::string(utf8.constData(), static_cast<std::size_t>(utf8.size())), options,
                 view_->page_count(), std::max(0, view_->current_page()));
}

void MainWindow::on_find_next() {
  if (!search_ || !search_->active()) {
    open_find();  // 입력이 있으면 검색을 시작한다
    return;
  }
  if (search_->next()) {
    if (search_->wrapped()) show_transient(tr("Searching again from the beginning"));
  } else if (search_->searching()) {
    show_transient(tr("Searching…"));
  }
}

void MainWindow::on_find_previous() {
  if (!search_ || !search_->active()) {
    open_find();
    return;
  }
  if (search_->previous()) {
    if (search_->wrapped()) show_transient(tr("Searching again from the end"));
  } else if (search_->searching()) {
    show_transient(tr("Searching…"));
  }
}

void MainWindow::on_search_closed() {
  if (search_) search_->clear();
  view_->setFocus();
}

void MainWindow::update_search_status() {
  if (!search_) {
    search_bar_->set_status(0, 0, false, false, false);
    return;
  }
  search_bar_->set_status(search_->current_ordinal(), search_->total(), search_->searching(),
                          search_->truncated(), search_->no_text());
}

void MainWindow::on_password_required(const QString& path) {
  if (!screenshot_path_.isEmpty()) {
    fail_screenshot(QStringLiteral("password required"));
    return;
  }
  if (print_unlock_) {  // 권한 암호가 틀렸다 — 기존 문서는 그대로다 (#23)
    print_unlock_.reset();
    prompt_print_unlock_password(tr("Wrong password."));
    return;
  }
  bool ok = false;
  const QString password =
      QInputDialog::getText(this, tr("Password Required"),
                            tr("Enter the password for %1:").arg(QFileInfo(path).fileName()),
                            QLineEdit::Password, QString(), &ok);
  if (!ok) {
    show_transient(tr("Cancel Opening"));
    settings_->clear_session_marker();  // (#131)
    return;
  }
  session_.open(path, password);
}

void MainWindow::on_open_failed(const QString& path, const QString& message) {
  settings_->clear_session_marker();  // (#131)
  print_unlock_.reset();
  if (!screenshot_path_.isEmpty()) {
    fail_screenshot(message);
    return;
  }
  show_transient(tr("Could not open: %1").arg(QFileInfo(path).fileName()));
  QMessageBox::warning(this, tr("This file cannot be opened"),
                       tr("%1\n\n%2").arg(QFileInfo(path).fileName(), message));
}

void MainWindow::on_first_view_rendered() {
  refresh_status();  // 타일이 도착한 뒤의 캐시·렌더 시간을 보여 준다
  if (screenshot_path_.isEmpty()) return;
  const QString path = screenshot_path_;
  screenshot_path_.clear();
  // 이번 paint 가 끝난 뒤 캡처
  QTimer::singleShot(0, this, [this, path] {
    if (!grab().save(path)) {
      fail_screenshot(path);
      return;
    }
    QApplication::exit(0);
  });
}

void MainWindow::on_copy() {
  bool truncated = false;
  const QString text = view_->selected_text(&truncated);
  if (text.isEmpty()) return;
  QGuiApplication::clipboard()->setText(text);
  const auto count = text.toUcs4().size();
  show_transient(
      truncated ? tr("Copied %n character(s) (the selection was too long, so only part was copied)",
                     nullptr, static_cast<int>(count))
                : tr("Copied %n character(s)", nullptr, static_cast<int>(count)));
}

void MainWindow::on_select_all() {
  view_->select_all_on_current_page();
}

void MainWindow::on_link_activated(const onpdf::view::Link& link) {
  if (link.kind == onpdf::view::LinkKind::kPage) {
    navigate_to(link);  // 링크는 location 이 이미 변환되어 있어 바로 이동한다
    return;
  }
  const QString uri = QString::fromUtf8(link.uri.data(), static_cast<qsizetype>(link.uri.size()));
  const QUrl url(uri);
  const bool well_formed =
      url.isValid() &&
      (url.scheme() == QStringLiteral("mailto") ? !url.path().isEmpty() : !url.host().isEmpty());
  if (!onpdf::view::is_safe_uri(link.uri) || !well_formed) {
    show_transient(tr("Blocked link: %1").arg(uri.left(80)));
    return;
  }
  const QString shown = uri.size() > 200 ? uri.left(200) + QStringLiteral("…") : uri;
  const auto answer = QMessageBox::question(
      this, tr("Open External Link"), tr("Open this external link?\n%1").arg(shown),
      QMessageBox::Open | QMessageBox::Cancel, QMessageBox::Cancel);
  if (answer == QMessageBox::Open && !QDesktopServices::openUrl(url))
    show_transient(tr("Could not open the link"));
}

void MainWindow::on_link_hovered(const QString& target) {
  link_label_->setText(target.isEmpty() ? QString() : tr("Link: %1").arg(target.left(80)));
}

void MainWindow::on_current_page_changed(int page) {
  if (page >= 0 && page_spin_->value() != page + 1) {
    const QSignalBlocker blocker(page_spin_);
    page_spin_->setValue(page + 1);
  }
  thumbnail_panel_->set_current_page(page);
  if (!outline_highlight_timer_->isActive()) outline_highlight_timer_->start();
  refresh_status();
}

void MainWindow::on_zoom_changed(double) {
  if (view_->has_document()) {  // "마지막 값" 시작 배율용 (#129)
    const ZoomMode mode = view_->zoom_mode();
    settings_->set_last_zoom(mode == ZoomMode::kFitPage  ? ZoomKind::kFitPage
                             : mode == ZoomMode::kCustom ? ZoomKind::kCustom
                                                         : ZoomKind::kFitWidth,
                             view_->zoom());
  }
  refresh_status();
}

// ---------------------------------------------------------------- 설정 (#129)

void MainWindow::apply_settings() {
  if (!settings_->writable() && !settings_write_warned_) {
    settings_write_warned_ = true;
    show_transient(tr("Settings cannot be saved"));
  }
  // 편집 기본값 — 값이 같으면 건드리지 않는다 (툴바 setter 가 설정에 쓴 뒤 돌아오는 changed)
  bool properties_changed = false;
  if (const double w = settings_->border_width(); w != shape_border_width_) {
    shape_border_width_ = w;
    properties_changed = true;
  }
  if (const std::uint8_t a = settings_->opacity_alpha(); a != shape_alpha_) {
    shape_alpha_ = a;
    properties_changed = true;
  }
  if (const engine::Color c = settings_->annotation_color(); c != annotation_color_) {
    annotation_color_ = c;
    if (color_button_ != nullptr) update_color_icon();
  }
  text_color_ = settings_->text_color();
  if (const double size = settings_->text_size(); size != text_size_) {
    text_size_ = size;
    properties_changed = true;
  }
  if (properties_changed) {
    sync_text_size_combo();
    update_property_buttons();
  }
  if (const int seconds = settings_->stall_seconds(); seconds != applied_stall_seconds_) {
    applied_stall_seconds_ = seconds;
    stall_threshold_ = std::chrono::seconds(seconds);
  }
  if (view_ != nullptr)
    view_->set_cache_limit(static_cast<std::size_t>(settings_->cache_mb()) << 20);
}

void MainWindow::sync_text_size_combo() {
  if (text_size_combo_ == nullptr) return;
  const int index = text_size_combo_->findData(static_cast<int>(std::lround(text_size_)));
  if (index >= 0 && index != text_size_combo_->currentIndex()) {
    QScopedValueRollback<bool> guard(syncing_text_size_, true);
    text_size_combo_->setCurrentIndex(index);
  }
}

void MainWindow::apply_start_zoom(StartZoom start, ZoomKind last_kind, double last_zoom) {
  switch (start) {
    case StartZoom::kFitWidth:
      view_->set_zoom_mode(ZoomMode::kFitWidth);
      break;
    case StartZoom::kFitPage:
      view_->set_zoom_mode(ZoomMode::kFitPage);
      break;
    case StartZoom::kActualSize:
      view_->set_zoom(1.0);
      break;
    case StartZoom::kLast:
      switch (last_kind) {
        case ZoomKind::kFitWidth:
          view_->set_zoom_mode(ZoomMode::kFitWidth);
          break;
        case ZoomKind::kFitPage:
          view_->set_zoom_mode(ZoomMode::kFitPage);
          break;
        case ZoomKind::kCustom:
          view_->set_zoom(last_zoom);
          break;
      }
      break;
  }
}

void MainWindow::restore_window_state() {
  const QByteArray geometry = settings_->window_geometry();
  if (geometry.isEmpty() || !restoreGeometry(geometry)) resize(900, 1000);
  const QByteArray splitter = settings_->splitter_state();
  if (!splitter.isEmpty()) splitter_->restoreState(splitter);
  bool visible = true;
  switch (settings_->sidebar_start()) {
    case SidebarStart::kVisible:
      visible = true;
      break;
    case SidebarStart::kHidden:
      visible = false;
      break;
    case SidebarStart::kLast:
      visible = settings_->sidebar_visible();
      break;
  }
  if (sidebar_action_ != nullptr) sidebar_action_->setChecked(visible);  // 액션을 트리거하지 않는다
  sidebar_->setVisible(visible);
}

void MainWindow::save_window_state() {
  settings_->set_window_geometry(saveGeometry());
  settings_->set_splitter_state(splitter_->saveState());
  settings_->sync();
}

void MainWindow::rebuild_recent_menu() {
  if (recent_menu_ == nullptr) return;
  recent_menu_->clear();
  const QStringList files = settings_->recent_files();
  for (const QString& path : files) {
    QAction* action = recent_menu_->addAction(QFileInfo(path).fileName());
    action->setToolTip(path);
    action->setData(path);
    connect(action, &QAction::triggered, this, [this, path] { open_file(path); });
  }
  if (files.isEmpty()) {
    QAction* none = recent_menu_->addAction(tr("(none)"));
    none->setEnabled(false);
  }
  recent_menu_->addSeparator();
  QAction* clear = recent_menu_->addAction(tr("&Clear List"), this, [this] {
    settings_->clear_recent();
    rebuild_recent_menu();
  });
  clear->setObjectName(QStringLiteral("recent_clear"));
  clear->setEnabled(!files.isEmpty());
}

QString MainWindow::start_dir(DirKind kind, const QString& preferred) const {
  if (!preferred.isEmpty() && QFileInfo(preferred).isDir()) return preferred;
  const QString last = settings_->last_dir(kind);
  return last.isEmpty() ? QDir::homePath() : last;
}

void MainWindow::remember_dir(DirKind kind, const QString& file_or_dir) {
  if (file_or_dir.isEmpty()) return;
  const QFileInfo info(file_or_dir);
  settings_->set_last_dir(kind, info.isDir() ? info.absoluteFilePath() : info.absolutePath());
}

void MainWindow::open_preferences() {
  SettingsDialog dialog(*settings_, this);
  connect(&dialog, &SettingsDialog::language_changed, this, [this] {
    auto* box = new QMessageBox(QMessageBox::Information, tr("Language"),
                                tr("The language will change the next time OnPdf starts."),
                                QMessageBox::Ok, this);
    box->setObjectName(QStringLiteral("language_restart_notice"));
    box->setWindowModality(Qt::NonModal);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->show();
  });
  dialog.exec();
}

// ---------------------------------------------------------------- 충돌 보고 (#131)

void MainWindow::set_action(const QString& action) {
  if (action == last_action_) return;
  last_action_ = action;
  settings_->set_session_last_action(action);
  crash::set_last_action(action.toUtf8().constData());
}

void MainWindow::open_report_folder() {
  QDir dir = report_dir();
  if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
    show_transient(tr("Cannot create the report folder: %1").arg(dir.absolutePath()));
    return;
  }
  if (!QDesktopServices::openUrl(QUrl::fromLocalFile(dir.absolutePath()))) {
    show_transient(tr("Cannot open the folder: %1").arg(dir.absolutePath()));
  }
}

bool MainWindow::maybe_show_crash_notice(const QString& cli_file) {
  const SessionMarker marker = settings_->session_marker();
  if (!marker.valid()) {
    if (!marker.stage.isEmpty() || !marker.path.isEmpty()) settings_->clear_session_marker();
    return true;
  }
  settings_->clear_session_marker();  // 안내는 한 번만
  const QString file_name = QFileInfo(marker.path).fileName();
  const bool same_as_cli =
      !cli_file.isEmpty() && QFileInfo(cli_file).absoluteFilePath() == marker.path;
  if (!settings_->crash_notice()) {
    show_transient(tr("OnPdf did not exit normally last time (%1)").arg(file_name));
    return true;
  }
  const std::optional<CrashRecord> record = latest_record_since(report_dir(), marker.since);
  const QString when = QLocale().toString(marker.since, QLocale::ShortFormat);
  QString text = tr("Last time, OnPdf quit unexpectedly while %1 '%2' (%3).")
                     .arg(stage_text(marker.stage), file_name, when);
  if (record) text += QLatin1Char('\n') + tr("Signal: %1").arg(record->signal_text);
  if (!marker.version.isEmpty() && marker.version != app_version())
    text += QLatin1Char('\n') + tr("That was version %1.").arg(marker.version);

  QString details =
      tr("Last action: %1")
          .arg(record && !record->last_action.isEmpty() ? record->last_action : marker.last_action);
  details += QLatin1Char('\n') + tr("Report folder: %1").arg(report_dir().absolutePath());
  if (const QString hint = system_report_hint(); !hint.isEmpty())
    details += QLatin1Char('\n') + tr("System crash reports: %1").arg(hint);
  details += QLatin1Char('\n') +
             tr("OnPdf %1 · PDFium %2 · Qt %3")
                 .arg(app_version(), pdfium_version(), QString::fromLatin1(qVersion()));

  const bool can_reopen = !marker.path.isEmpty() && QFileInfo::exists(marker.path);
  bool open_again = false;
  bool dont_show = false;
  for (;;) {  // ActionRole 버튼은 상자를 닫으므로 폴더·URL 을 연 뒤 다시 띄운다
    QMessageBox box(QMessageBox::Warning, tr("OnPdf did not exit normally"), text,
                    QMessageBox::NoButton, this);
    box.setObjectName(QStringLiteral("crash_notice"));
    box.setInformativeText(
        tr("The file may contain content PDFium cannot handle. You can open it again or leave "
           "it closed. The report folder holds what you can attach to a bug report."));
    box.setDetailedText(details);
    auto* check = new QCheckBox(tr("Do not show this again"), &box);
    check->setObjectName(QStringLiteral("crash_notice_dont_show"));
    check->setChecked(dont_show);
    box.setCheckBox(check);
    QPushButton* reopen =
        can_reopen ? box.addButton(tr("Open It Again"), QMessageBox::AcceptRole) : nullptr;
    if (reopen != nullptr) reopen->setObjectName(QStringLiteral("crash_notice_open"));
    QPushButton* leave = box.addButton(tr("Leave It Closed"), QMessageBox::RejectRole);
    leave->setObjectName(QStringLiteral("crash_notice_leave"));
    QPushButton* folder = box.addButton(tr("Open Report Folder"), QMessageBox::ActionRole);
    folder->setObjectName(QStringLiteral("crash_notice_folder"));
    QPushButton* issue = box.addButton(tr("Report an Issue..."), QMessageBox::ActionRole);
    issue->setObjectName(QStringLiteral("crash_notice_issue"));
    box.setDefaultButton(leave);
    box.setEscapeButton(leave);
    box.exec();
    dont_show = check->isChecked();
    QAbstractButton* clicked = box.clickedButton();
    if (clicked == folder) {
      open_report_folder();
      continue;
    }
    if (clicked == issue) {
      const QUrl url = issue_url(marker, record);
      if (!QDesktopServices::openUrl(url)) {
        QApplication::clipboard()->setText(url.toString());
        show_transient(tr("Cannot open the browser; the issue link was copied to the clipboard"));
      }
      continue;
    }
    open_again = reopen != nullptr && clicked == reopen;
    break;
  }
  if (dont_show) settings_->set_crash_notice(false);
  if (open_again) {
    const QString path = marker.path;
    QTimer::singleShot(0, this, [this, path] { open_file(path); });
  }
  return !same_as_cli;  // 같은 파일: "다시 열기" 면 이미 예약했고, "열지 않기" 면 열지 않는다
}

// ---------------------------------------------------------------- 워커 멈춤 감시 (#121)

bool MainWindow::worker_stalled() const {
  const auto running = worker_.running_task();
  if (!running || running->kind != engine::TaskKind::kRender) return false;
  return std::chrono::steady_clock::now() - running->since >= stall_threshold_;
}

void MainWindow::check_worker_stall() {
  const auto running = worker_.running_task();
  if (!running || running->kind != engine::TaskKind::kRender ||
      std::chrono::steady_clock::now() - running->since < stall_threshold_) {
    // 작업이 끝났거나 바뀌었다: 경고 상태를 풀고 상자가 떠 있으면 닫는다 (복구)
    if (warned_since_ && (!running || running->since != *warned_since_)) {
      warned_since_.reset();
      if (stall_box_) stall_box_->close();
    }
    return;
  }
  if (warned_since_ && *warned_since_ == running->since) return;  // 같은 작업은 한 번만
  show_stall_warning();
}

void MainWindow::show_stall_warning() {
  const auto running = worker_.running_task();
  if (!running) return;
  warned_since_ = running->since;
  if (stall_box_) {
    stall_box_->raise();
    stall_box_->activateWindow();
    return;
  }
  const auto seconds = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                                            std::chrono::steady_clock::now() - running->since)
                                            .count());
  const QString file = session_.has_document() ? QFileInfo(session_.path()).fileName() : QString();
  const QString where =
      running->page >= 0 ? tr("page %1 of '%2'").arg(running->page + 1).arg(file) : file;
  auto* box = new QMessageBox(
      QMessageBox::Warning, tr("Not responding"),
      tr("Rendering %1 has not finished for %n second(s). The document may contain hostile "
         "content.\nIf you force quit, unsaved changes are lost.",
         nullptr, seconds)
          .arg(where),
      QMessageBox::NoButton, this);
  box->setObjectName(QStringLiteral("worker_stalled"));
  box->setWindowModality(Qt::WindowModal);  // 선택할 때까지 다른 조작을 막는다
  box->setAttribute(Qt::WA_DeleteOnClose);
  QPushButton* quit = box->addButton(tr("Force quit"), QMessageBox::DestructiveRole);
  box->addButton(tr("Keep waiting"), QMessageBox::RejectRole);
  connect(box, &QMessageBox::buttonClicked, this, [this, quit](QAbstractButton* clicked) {
    if (clicked != quit) return;
    // 다음 실행이 "강제 종료" 로 안내하게 표식을 남긴다 (#131). 핸들러는 _Exit 를 거치지 않는다
    if (const auto running = worker_.running_task(); running && running->page >= 0)
      settings_->set_session_last_action(QStringLiteral("render p.%1").arg(running->page + 1));
    settings_->set_session_stage(QStringLiteral("forced-quit"));
    force_exit_(3);  // 워커가 join 되지 않으므로 소멸자 없이 끝낸다
  });
  stall_box_ = box;
  box->show();
}

void MainWindow::on_page_refused(int page, const QString& reason) {
  show_transient(tr("Page %1 cannot be displayed: %2").arg(page + 1).arg(reason));
}

void MainWindow::on_tool_blocked(int page) {
  show_transient(tr("Page %1 cannot be displayed, so annotations, text and images cannot be added")
                     .arg(page + 1));
}

void MainWindow::show_hazard_notice() {
  const auto& hazards = session_.handle().info().hazards;
  if (hazards.empty()) return;
  if (!settings_->hazard_notice()) {  // 설정에서 껐으면 상태 표시줄 한 줄만 (#129)
    show_transient(tr("%n page(s) cannot be displayed (hazardous pattern)", nullptr,
                      static_cast<int>(hazards.size())));
    return;
  }
  QStringList pages;
  for (const auto& h : hazards) pages << QString::number(h.page + 1);
  auto* box = new QMessageBox(
      QMessageBox::Information, tr("Pages that cannot be displayed"),
      tr("%n page(s) of this document contain a pattern that cannot be rendered safely and "
         "will stay blank: %1.\nOther pages, search, editing and saving work as usual.",
         nullptr, static_cast<int>(hazards.size()))
          .arg(pages.join(QStringLiteral(", "))),
      QMessageBox::Ok, this);
  box->setObjectName(QStringLiteral("render_hazards_notice"));
  box->setWindowModality(Qt::NonModal);
  box->setAttribute(Qt::WA_DeleteOnClose);
  box->show();
}

void MainWindow::refresh_status() {
  if (!session_.has_document() || !view_->has_document()) return;
  const auto cache = view_->cache_stats();
  const auto sched = view_->scheduler_stats();
  set_document_status(tr("%1 · %2% · cache %3 MB · tile %4 ms")
                          .arg(QFileInfo(session_.path()).fileName())
                          .arg(static_cast<int>(view_->zoom() * 100.0 + 0.5))
                          .arg(static_cast<double>(cache.bytes) / (1024.0 * 1024.0), 0, 'f', 1)
                          .arg(sched.avg_render_ms, 0, 'f', 1));
}

void MainWindow::set_document_status(const QString& text) {
  status_label_->setText(text);
}

void MainWindow::show_transient(const QString& text) {
  statusBar()->showMessage(text, 5000);
  selection_status_shown_ = false;
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
  if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event) {
  const QList<QUrl> urls = event->mimeData()->urls();
  if (urls.isEmpty()) return;
  const QString path = urls.first().toLocalFile();
  if (looks_like_image_file(path) && view_->has_document()) {
    // 이미지 파일: 드롭한 자리에 놓는다 (#49). PDF 는 기존대로 연다
    const QPointF at = view_->viewport()->mapFrom(this, event->position().toPoint());
    if (auto hit = view_->page_point_at(at)) {
      event->acceptProposedAction();
      const auto [page, pt] = *hit;
      QTimer::singleShot(0, this, [this, path, page, pt] { add_image_file(path, page, pt, true); });
      return;
    }
  }
  if (!path.toLower().endsWith(QStringLiteral(".pdf"))) {
    show_transient(tr("Only PDF files can be opened"));
    return;
  }
  event->acceptProposedAction();
  // 열기는 다음 이벤트 턴에: open_file 은 인쇄 중이면 모달 확인을 띄우는데, drop 처리 안에서 중첩
  // 이벤트 루프를 돌리면 끌기 원본(macOS Finder·Windows OLE)이 drop 완료를 기다리며 멈춘다 (#27)
  QTimer::singleShot(0, this, [this, path] { open_file(path); });
}

}  // namespace onpdf::app
