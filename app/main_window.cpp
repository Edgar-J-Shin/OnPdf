#include "main_window.h"

#include "document_view.h"
#include "onpdf/core/platform/path.h"
#include "annotation_dialog.h"
#include "onpdf/core/engine/qpdf_tools.h"
#include "onpdf/core/engine/save.h"
#include "onpdf/core/page_ranges.h"
#include "onpdf/edit/annotation_commands.h"
#include "onpdf/edit/delete_pages.h"
#include "onpdf/edit/insert_pages.h"
#include "onpdf/edit/move_pages.h"
#include "onpdf/edit/page_ops.h"
#include "onpdf/edit/rotate_page.h"
#include "outline_panel.h"
#include "search_bar.h"
#include "thumbnail_panel.h"

#include <QAction>
#include <QActionGroup>
#include <QPainter>
#include <QPixmap>
#include <QToolBar>
#include <QToolButton>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPointer>
#include <QPrintDialog>
#include <QPushButton>
#include <QPrinter>
#include <QProgressBar>
#include <QProgressDialog>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <string>

namespace onpdf::app {

MainWindow::MainWindow(engine::PdfiumWorker& worker, UiDispatcher& dispatcher, QWidget* parent)
    : QMainWindow(parent), dispatcher_(dispatcher), session_(worker, dispatcher, this) {
  setWindowTitle(QStringLiteral("OnPdf"));
  setAcceptDrops(true);

  auto* central = new QWidget(this);
  auto* column = new QVBoxLayout(central);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(0);
  search_bar_ = new SearchBar(central);
  view_ = new DocumentView(central);
  view_->clear_document(tr("파일을 열거나 여기로 끌어다 놓으세요 (Ctrl+O)"));
  column->addWidget(search_bar_);
  column->addWidget(view_, 1);

  // 사이드바(썸네일·목차) | 문서
  sidebar_ = new QTabWidget(this);
  sidebar_->setDocumentMode(true);
  thumbnail_panel_ = new ThumbnailPanel(sidebar_);
  outline_panel_ = new OutlinePanel(sidebar_);
  sidebar_->addTab(thumbnail_panel_, tr("썸네일"));
  sidebar_->addTab(outline_panel_, tr("목차"));
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
  set_document_status(tr("문서 없음"));

  edit_ = new EditController(dispatcher_, this);
  structure_job_ = new StructureJob(dispatcher_, this);
  connect(structure_job_, &StructureJob::finished, this, &MainWindow::on_structure_finished);
  build_menus();
  build_annotation_ui();
  connect(edit_, &EditController::document_changed, this, &MainWindow::on_document_changed);
  connect(view_, &DocumentView::markup_requested, this, &MainWindow::on_markup_requested);
  connect(view_, &DocumentView::note_requested, this, &MainWindow::on_note_requested);
  connect(view_, &DocumentView::ink_requested, this, &MainWindow::on_ink_requested);
  connect(view_, &DocumentView::shape_requested, this, &MainWindow::on_shape_requested);
  connect(view_, &DocumentView::annotation_selected, this, &MainWindow::on_annotation_selected);
  connect(view_, &DocumentView::annotation_edit_requested, this, &MainWindow::on_annotation_edit_requested);
  connect(view_, &DocumentView::annotation_delete_requested, this, [this](int, const QString&) { delete_selected_annotation(); });
  connect(edit_, &EditController::state_changed, this, [this] {
    update_edit_actions();
    update_title();
  });
  connect(edit_, &EditController::failed, this, &MainWindow::on_edit_failed);

  connect(&session_, &DocumentSession::opened, this, &MainWindow::on_opened);
  connect(&session_, &DocumentSession::password_required, this, &MainWindow::on_password_required);
  connect(&session_, &DocumentSession::open_failed, this, &MainWindow::on_open_failed);
  connect(view_, &DocumentView::current_page_changed, this, &MainWindow::on_current_page_changed);
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
  connect(thumbnail_panel_, &ThumbnailPanel::context_menu_requested, this, &MainWindow::on_thumbnail_context_menu);
  connect(outline_panel_, &OutlinePanel::item_activated, this, &MainWindow::on_outline_item);
  connect(view_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
    // 스로틀: 진행 중이면 다시 시작하지 않는다 → 연속(관성) 스크롤 중에도 100ms 마다 갱신
    if (sidebar_->isVisible() && sidebar_->currentWidget() == outline_panel_ && !outline_highlight_timer_->isActive()) {
      outline_highlight_timer_->start();
    }
  });
  connect(sidebar_, &QTabWidget::currentChanged, this, [this] { update_outline_highlight(); });
  connect(page_spin_, &QSpinBox::valueChanged, this, [this](int value) {
    if (view_->has_document() && view_->current_page() != value - 1) view_->go_to_page(value - 1);
  });

  resize(900, 1000);
}

MainWindow::~MainWindow() {
  print_job_.reset();  // 신호 없이 취소 (printer_ 보다 먼저)
  view_->set_search(nullptr);  // 세션이 먼저 사라지므로 뷰가 댕글 포인터를 들지 않게
}

void MainWindow::build_menus() {
  QMenu* file = menuBar()->addMenu(tr("파일(&F)"));
  QAction* open = file->addAction(tr("열기(&O)..."), QKeySequence::Open, this, &MainWindow::on_open_action);
  open->setStatusTip(tr("PDF 파일을 연다"));
  save_action_ = file->addAction(tr("저장(&S)"), QKeySequence::Save, this, &MainWindow::on_save);
  save_as_action_ = file->addAction(tr("다른 이름으로 저장(&A)..."), QKeySequence::SaveAs, this, &MainWindow::on_save_as);
  file->addSeparator();
  merge_action_ = file->addAction(tr("병합(&M)..."), this, &MainWindow::on_merge_action);
  merge_action_->setStatusTip(tr("PDF 여러 개를 한 파일로 합친다 (첫 문서의 목차·폼 유지)"));
  split_action_ = file->addAction(tr("분할(&T)..."), this, &MainWindow::on_split_action);
  split_action_->setStatusTip(tr("현재 문서를 N쪽마다 나눠 저장한다"));
  split_action_->setEnabled(false);
  file->addSeparator();
  print_action_ = file->addAction(tr("인쇄(&P)..."), QKeySequence::Print, this, &MainWindow::on_print);
  print_action_->setEnabled(false);
  file->addSeparator();
  // QKeySequence::Quit 은 Windows 에서 비어 있으므로 Ctrl+Q 를 명시한다
  // 종료는 창 닫기로: closeEvent 에서 저장 확인을 거친다 (마지막 창이 닫히면 앱이 끝난다)
  QAction* quit = file->addAction(tr("종료(&Q)"), QKeySequence(Qt::CTRL | Qt::Key_Q), this, &QWidget::close);
  quit->setMenuRole(QAction::QuitRole);

  QMenu* edit = menuBar()->addMenu(tr("편집(&E)"));
  undo_action_ = edit->addAction(tr("실행 취소(&U)"), QKeySequence::Undo, this, [this] { edit_->undo(); });
  redo_action_ = edit->addAction(tr("다시 실행(&R)"), QKeySequence::Redo, this, [this] { edit_->redo(); });
  edit->addSeparator();
  copy_action_ = edit->addAction(tr("복사(&C)"), QKeySequence::Copy, this, &MainWindow::on_copy);
  copy_action_->setEnabled(false);
  select_all_action_ = edit->addAction(tr("현재 쪽 전체 선택(&A)"), QKeySequence::SelectAll, this,
                                       &MainWindow::on_select_all);
  edit->addSeparator();
  edit->addAction(tr("찾기(&F)..."), QKeySequence::Find, this, &MainWindow::open_find);
  find_next_action_ = edit->addAction(tr("다음 찾기(&N)"), QKeySequence::FindNext, this, &MainWindow::on_find_next);
  find_previous_action_ =
      edit->addAction(tr("이전 찾기(&P)"), QKeySequence::FindPrevious, this, &MainWindow::on_find_previous);

  QMenu* page = menuBar()->addMenu(tr("쪽(&P)"));
  rotate_cw_action_ = page->addAction(tr("오른쪽으로 회전(&R)"), QKeySequence(Qt::CTRL | Qt::Key_R), this,
                                      [this] { rotate_current_page(1); });
  rotate_ccw_action_ = page->addAction(tr("왼쪽으로 회전(&L)"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R), this,
                                       [this] { rotate_current_page(3); });
  page->addSeparator();
  // Delete 단축키는 썸네일 목록이 직접 처리한다 (목록 밖에서는 현재 쪽). 메뉴에 단축키를 두면 목록의 키와 겹친다
  delete_pages_action_ = page->addAction(tr("쪽 삭제(&D)"), this, [this] { delete_pages(target_pages()); });
  move_up_action_ = page->addAction(tr("위로 이동(&U)"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Up), this,
                                    [this] { move_selection(-1); });
  move_down_action_ = page->addAction(tr("아래로 이동(&W)"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Down), this,
                                      [this] { move_selection(+1); });
  page->addSeparator();
  insert_pages_action_ = page->addAction(tr("파일에서 쪽 삽입(&I)..."), this, &MainWindow::on_insert_pages_action);

  QMenu* view = menuBar()->addMenu(tr("보기(&V)"));
  view->addAction(tr("너비 맞춤(&W)"), QKeySequence(Qt::CTRL | Qt::Key_1), this,
                  [this] { view_->set_zoom_mode(ZoomMode::kFitWidth); });
  view->addAction(tr("쪽 맞춤(&P)"), QKeySequence(Qt::CTRL | Qt::Key_2), this,
                  [this] { view_->set_zoom_mode(ZoomMode::kFitPage); });
  view->addAction(tr("실제 크기(&A)"), QKeySequence(Qt::CTRL | Qt::Key_0), this, [this] { view_->set_zoom(1.0); });
  view->addSeparator();
  view->addAction(tr("확대(&I)"), QKeySequence::ZoomIn, this, [this] { view_->zoom_in(); });
  view->addAction(tr("축소(&O)"), QKeySequence::ZoomOut, this, [this] { view_->zoom_out(); });
  view->addSeparator();
  sidebar_action_ = view->addAction(tr("사이드바(&S)"), QKeySequence(Qt::Key_F4), this, [this](bool checked) {
    sidebar_->setVisible(checked);
    if (checked) update_outline_highlight();
  });
  sidebar_action_->setCheckable(true);
  sidebar_action_->setChecked(true);

  QMenu* help = menuBar()->addMenu(tr("도움말(&H)"));
  help->addAction(tr("OnPdf 정보(&A)"), this, [this] {
    QMessageBox::about(this, tr("OnPdf 정보"),
                       tr("OnPdf %1 — PDFium 기반 PDF 뷰어·편집기\nApache License 2.0")
                           .arg(QApplication::applicationVersion()));
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
  // 수정된 문서면 먼저 저장 여부를 묻고, 그다음 인쇄 확인 (순서: 수정 → 인쇄)
  maybe_save([this, path] { continue_open(path); });
}

void MainWindow::continue_open(const QString& path) {
  // 인쇄 중이면 먼저 묻는다: 그대로 두면 열지 않고, 취소하고 열기를 고르면 인쇄를 취소한 뒤 연다
  if (print_job_ && print_job_->active() && !confirm_cancel_print_for(path)) {
    show_transient(tr("인쇄 중이라 열지 않았습니다"));
    return;
  }
  show_transient(tr("여는 중: %1").arg(QFileInfo(path).fileName()));
  session_.open(path);
}

void MainWindow::on_open_action() {
  const QString path = QFileDialog::getOpenFileName(this, tr("PDF 열기"), QString(),
                                                    tr("PDF 문서 (*.pdf);;모든 파일 (*)"));
  if (!path.isEmpty()) open_file(path);
}

void MainWindow::on_opened(const QString& path, int page_count) {
  edit_->set_document(page_count > 0 ? session_.handle() : engine::DocumentHandle{});
  update_title();
  statusBar()->clearMessage();  // "여는 중" 임시 메시지가 문서 정보를 가리지 않게
  if (page_count <= 0) {
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
    view_->clear_document(tr("쪽이 없는 문서입니다"));
    page_spin_->setEnabled(false);
    page_total_label_->clear();
    set_document_status(tr("%1 · 0 쪽").arg(QFileInfo(path).fileName()));
    return;
  }
  page_spin_->setRange(1, page_count);
  page_spin_->setEnabled(true);
  page_total_label_->setText(tr("/ %1 쪽").arg(page_count));
  view_->set_document(session_.handle(), dispatcher_);
  view_->setFocus();
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
}

// ---------------------------------------------------------------- 편집·저장

// 주석 속성 표시용 (두께는 도형·펜만)
namespace {
bool has_border_width(engine::AnnotationKind kind) noexcept {
  return kind == engine::AnnotationKind::kInk || kind == engine::AnnotationKind::kSquare ||
         kind == engine::AnnotationKind::kCircle;
}
QString width_text(double width) { return MainWindow::tr("%1pt").arg(width); }
QString opacity_text(double opacity) { return MainWindow::tr("%1%").arg(qRound(opacity * 100.0)); }
}  // namespace

QString MainWindow::command_label(std::string_view id) const {
  if (id == "rotate_page") return tr("쪽 회전");
  if (id == "delete_pages") return tr("쪽 삭제");
  if (id == "move_pages") return tr("쪽 이동");
  if (id == "insert_pages") return tr("쪽 삽입");
  if (id == "add_highlight") return tr("하이라이트 추가");
  if (id == "add_underline") return tr("밑줄 추가");
  if (id == "add_strikeout") return tr("취소선 추가");
  if (id == "add_note") return tr("메모 추가");
  if (id == "add_ink") return tr("펜 추가");
  if (id == "add_square") return tr("사각형 추가");
  if (id == "add_circle") return tr("원 추가");
  if (id == "set_annotation_border_width") return tr("선 두께 변경");
  if (id == "set_annotation_opacity") return tr("불투명도 변경");
  if (id == "remove_annotation") return tr("주석 삭제");
  if (id == "set_annotation_color") return tr("주석 색 변경");
  if (id == "set_annotation_contents") return tr("메모 수정");
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
  const bool has_pages = session_.has_document() && session_.handle().valid() && session_.handle().info().page_count > 0;
  const bool printing = print_job_ && print_job_->active();
  // 편집 가능 상태(작업 중이 아님)와 편집 종류별 문서 권한을 따로 본다
  const bool idle = has_pages && !edit_->busy() && !saving_ && !printing;
  const bool can_assemble = has_pages && session_.handle().info().permissions.assemble;  // 쪽 회전·삭제·삽입
  rotate_cw_action_->setEnabled(idle && can_assemble);
  rotate_ccw_action_->setEnabled(idle && can_assemble);
  const QString assemble_tip = has_pages && !can_assemble ? tr("이 문서는 쪽 편집(회전·삭제·삽입)이 허용되지 않습니다")
                                                          : QString();
  rotate_cw_action_->setStatusTip(assemble_tip);
  rotate_ccw_action_->setStatusTip(assemble_tip);
  for (QAction* action : {delete_pages_action_, move_up_action_, move_down_action_, insert_pages_action_}) {
    action->setEnabled(idle && can_assemble);
    action->setStatusTip(assemble_tip);
  }
  thumbnail_panel_->set_editable(idle && can_assemble);
  const bool can_annotate = has_pages && session_.handle().info().permissions.annotate;
  const QString annotate_tip = has_pages && !can_annotate ? tr("이 문서는 주석이 허용되지 않습니다") : QString();
  // 도구는 권한으로만 켜고 끈다. 바쁨·저장·인쇄 중에는 요청(annotation_edits_allowed_)만 막아 도구가 유지된다 (리뷰 W1)
  annotation_edits_allowed_ = idle && can_annotate;
  for (QAction* action : tool_actions_) {
    if (action == nullptr) continue;
    const bool select = action == tool_actions_[0];
    action->setEnabled(select || can_annotate);
    action->setStatusTip(select ? QString() : annotate_tip);
  }
  if (!can_annotate && view_->tool() != AnnotationTool::kSelect) {  // 권한을 잃으면(문서 교체) 선택 도구로
    tool_actions_[0]->setChecked(true);
    view_->set_tool(AnnotationTool::kSelect);
  }
  for (QAction* action : markup_selection_actions_) {
    if (action != nullptr) action->setEnabled(idle && can_annotate && view_->has_selection());
  }
  delete_annotation_action_->setEnabled(annotation_edits_allowed_ && view_->selected_annotation().has_value());
  color_button_->setEnabled(can_annotate);
  if (width_button_ != nullptr) {
    const auto selected = view_->selected_annotation();
    width_button_->setEnabled(can_annotate && (!selected || has_border_width(selected->kind)));
    opacity_button_->setEnabled(can_annotate);
  }
  const bool structure_busy = structure_job_ != nullptr && structure_job_->active();
  split_action_->setEnabled(has_pages && !structure_busy);
  merge_action_->setEnabled(!structure_busy);
  // 실행 취소 대상은 이미 허용된 편집이므로 작업 중이 아니면 된다
  undo_action_->setEnabled(idle && edit_->can_undo());
  redo_action_->setEnabled(idle && edit_->can_redo());
  undo_action_->setText(edit_->can_undo() ? tr("실행 취소: %1(&U)").arg(command_label(edit_->undo_id()))
                                          : tr("실행 취소(&U)"));
  redo_action_->setText(edit_->can_redo() ? tr("다시 실행: %1(&R)").arg(command_label(edit_->redo_id()))
                                          : tr("다시 실행(&R)"));
  save_action_->setEnabled(session_.has_document() && edit_->is_modified() && !saving_ && !edit_->busy());
  save_as_action_->setEnabled(has_pages && !saving_ && !edit_->busy());
}

void MainWindow::rotate_current_page(int quarter_turns) { rotate_pages(target_pages(), quarter_turns); }

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
  QMessageBox box(QMessageBox::Warning, tr("서명된 문서"),
                  tr("이 문서에는 전자서명이 있습니다. 쪽이나 주석을 바꾸면 서명이 무효화될 수 있습니다.\n계속할까요?"),
                  QMessageBox::NoButton, this);
  box.setObjectName(QStringLiteral("signature_warning"));
  QPushButton* go = box.addButton(tr("계속"), QMessageBox::AcceptRole);
  QPushButton* cancel = box.addButton(tr("취소"), QMessageBox::RejectRole);
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
    show_transient(tr("문서에는 적어도 한 쪽이 있어야 합니다"));
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
  for (std::size_t i = 0; i < order.size(); ++i) identity = identity && order[i] == static_cast<int>(i);
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
  const QString path = QFileDialog::getOpenFileName(this, tr("쪽을 가져올 PDF"), QFileInfo(session_.path()).absolutePath(),
                                                    tr("PDF 문서 (*.pdf)"));
  if (path.isEmpty()) return;
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
  if (pending_insert_ && code == ErrorCode::kPasswordRequired) {
    // 삽입 소스가 암호 문서: 묻고 같은 삽입을 다시 시도한다 (취소하면 끝)
    const PendingInsert pending = *pending_insert_;
    pending_insert_.reset();
    QInputDialog dialog(this);
    dialog.setObjectName(QStringLiteral("insert_password"));
    dialog.setWindowTitle(tr("암호"));
    dialog.setLabelText(tr("%1 은(는) 암호로 보호되어 있습니다. 암호를 입력하세요.").arg(QFileInfo(pending.path).fileName()));
    dialog.setTextEchoMode(QLineEdit::Password);
    if (dialog.exec() != QDialog::Accepted || dialog.textValue().isEmpty()) return;
    insert_pages_from(pending.path, pending.at, dialog.textValue());
    return;
  }
  pending_insert_.reset();
  if (code == ErrorCode::kPermissionDenied) {
    show_transient(tr("이 문서는 쪽 추출이 허용되지 않아 삽입할 수 없습니다"));
    return;
  }
  show_transient(tr("편집하지 못했습니다: %1").arg(message));
}

// ---------------------------------------------------------------- 주석

namespace {

constexpr engine::Color kPalette[] = {
    {255, 255, 0, 255}, {0, 200, 0, 255}, {0, 150, 255, 255}, {255, 105, 180, 255}, {255, 0, 0, 255}, {255, 140, 0, 255},
};
const char* const kPaletteNames[] = {QT_TR_NOOP("노랑"), QT_TR_NOOP("초록"), QT_TR_NOOP("파랑"),
                                     QT_TR_NOOP("분홍"), QT_TR_NOOP("빨강"), QT_TR_NOOP("주황")};

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
    case engine::AnnotationKind::kHighlight: return MainWindow::tr("하이라이트");
    case engine::AnnotationKind::kUnderline: return MainWindow::tr("밑줄");
    case engine::AnnotationKind::kStrikeOut: return MainWindow::tr("취소선");
    case engine::AnnotationKind::kSquiggly: return MainWindow::tr("물결선");
    case engine::AnnotationKind::kText: return MainWindow::tr("메모");
    case engine::AnnotationKind::kInk: return MainWindow::tr("펜");
    case engine::AnnotationKind::kSquare: return MainWindow::tr("사각형");
    case engine::AnnotationKind::kCircle: return MainWindow::tr("원");
    case engine::AnnotationKind::kLink: return MainWindow::tr("링크");
    case engine::AnnotationKind::kOther: break;
  }
  return MainWindow::tr("주석");
}

// PDF 날짜 "D:YYYYMMDDHHmmSS…" → "YYYY-MM-DD"
QString short_date(const std::string& pdf_date) {
  if (pdf_date.size() < 10 || pdf_date.rfind("D:", 0) != 0) return QString::fromStdString(pdf_date);
  const QString d = QString::fromStdString(pdf_date.substr(2, 8));
  return d.left(4) + QLatin1Char('-') + d.mid(4, 2) + QLatin1Char('-') + d.mid(6, 2);
}

}  // namespace

void MainWindow::build_annotation_ui() {
  annotation_toolbar_ = addToolBar(tr("주석"));
  annotation_toolbar_->setObjectName(QStringLiteral("annotation_toolbar"));
  annotation_toolbar_->setMovable(false);
  annotation_toolbar_->setToolButtonStyle(Qt::ToolButtonTextOnly);
  QMenu* menu = menuBar()->addMenu(tr("주석(&N)"));
  tool_group_ = new QActionGroup(this);
  tool_group_->setExclusive(true);
  const QString names[kAnnotationToolCount] = {tr("선택(&S)"), tr("하이라이트(&H)"), tr("밑줄(&U)"), tr("취소선(&K)"),
                                               tr("메모(&N)"), tr("펜(&P)"), tr("사각형(&R)"), tr("원(&O)")};
  const QString tips[kAnnotationToolCount] = {tr("텍스트를 선택하거나 주석을 고른다"), tr("드래그한 글에 하이라이트"),
                                              tr("드래그한 글에 밑줄"), tr("드래그한 글에 취소선"), tr("클릭한 자리에 메모"),
                                              tr("드래그로 자유 선"), tr("드래그한 영역에 사각형 테두리"), tr("드래그한 영역에 원")};
  for (int i = 0; i < kAnnotationToolCount; ++i) {
    QAction* action = tool_group_->addAction(names[i]);
    action->setCheckable(true);
    action->setStatusTip(tips[i]);
    action->setChecked(i == 0);
    connect(action, &QAction::triggered, this, [this, i](bool checked) {
      if (checked) view_->set_tool(static_cast<AnnotationTool>(i));
    });
    tool_actions_[i] = action;
    annotation_toolbar_->addAction(action);
    menu->addAction(action);
  }
  annotation_toolbar_->addSeparator();
  menu->addSeparator();
  color_button_ = new QToolButton(annotation_toolbar_);
  color_button_->setObjectName(QStringLiteral("annotation_color"));
  color_button_->setToolTip(tr("주석 색"));
  color_button_->setPopupMode(QToolButton::InstantPopup);
  auto* palette = new QMenu(color_button_);
  QMenu* color_menu = menu->addMenu(tr("색(&C)"));
  for (int i = 0; i < 6; ++i) {
    const engine::Color c = kPalette[i];
    for (QMenu* m : {palette, color_menu}) {
      QAction* a = m->addAction(color_icon(c), tr(kPaletteNames[i]));
      connect(a, &QAction::triggered, this, [this, c] { set_annotation_color(c); });
    }
  }
  color_button_->setMenu(palette);
  update_color_icon();
  annotation_toolbar_->addWidget(color_button_);
  // 두께 ▾ (도형·펜) · 불투명도 ▾ — 선택이 있으면 그 주석, 없으면 새 도형·펜의 기본값 (색 버튼과 같은 규칙)
  width_button_ = new QToolButton(annotation_toolbar_);
  width_button_->setObjectName(QStringLiteral("annotation_width"));
  width_button_->setToolTip(tr("선 두께 (도형·펜)"));
  width_button_->setPopupMode(QToolButton::InstantPopup);
  auto* width_popup = new QMenu(width_button_);
  QMenu* width_menu = menu->addMenu(tr("두께(&W)"));
  for (double w : {1.0, 2.0, 4.0, 8.0}) {
    for (QMenu* m : {width_popup, width_menu}) {
      QAction* a = m->addAction(tr("%1pt").arg(w));
      a->setData(w);
      connect(a, &QAction::triggered, this, [this, w] { set_annotation_border_width(w); });
    }
  }
  width_button_->setMenu(width_popup);
  annotation_toolbar_->addWidget(width_button_);
  opacity_button_ = new QToolButton(annotation_toolbar_);
  opacity_button_->setObjectName(QStringLiteral("annotation_opacity"));
  opacity_button_->setToolTip(tr("불투명도"));
  opacity_button_->setPopupMode(QToolButton::InstantPopup);
  auto* opacity_popup = new QMenu(opacity_button_);
  QMenu* opacity_menu = menu->addMenu(tr("불투명도(&O)"));
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
  update_property_buttons();
  annotation_toolbar_->addSeparator();
  delete_annotation_action_ = annotation_toolbar_->addAction(tr("주석 삭제"), this, [this] { delete_selected_annotation(); });
  delete_annotation_action_->setStatusTip(tr("선택한 주석을 지운다 (Delete)"));
  delete_annotation_action_->setEnabled(false);
  menu->addAction(delete_annotation_action_);
  menu->addSeparator();
  const engine::AnnotationKind kinds[3] = {engine::AnnotationKind::kHighlight, engine::AnnotationKind::kUnderline,
                                           engine::AnnotationKind::kStrikeOut};
  const QString sel_names[3] = {tr("선택 영역 하이라이트"), tr("선택 영역 밑줄"), tr("선택 영역 취소선")};
  const QKeySequence keys[3] = {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_U),
                                QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K)};
  for (int i = 0; i < 3; ++i) {
    const engine::AnnotationKind kind = kinds[i];
    markup_selection_actions_[i] = menu->addAction(sel_names[i], keys[i], this, [this, kind] { add_markup_from_selection(kind); });
    markup_selection_actions_[i]->setEnabled(false);
  }
}

QAction* MainWindow::tool_action(AnnotationTool tool) const { return tool_actions_[static_cast<int>(tool)]; }

void MainWindow::update_color_icon() {
  color_button_->setIcon(color_icon(annotation_color_));
  color_button_->setText(tr("색"));
}

void MainWindow::update_property_buttons() {
  if (width_button_ == nullptr || opacity_button_ == nullptr) return;
  // 선택이 있으면 그 주석의 값, 없으면 새 도형·펜의 기본값
  if (auto selected = view_->selected_annotation(); selected) {
    width_button_->setText(has_border_width(selected->kind) ? width_text(selected->has_border ? selected->border_width : 1.0)
                                                            : tr("두께"));
    opacity_button_->setText(opacity_text(selected->opacity));
  } else {
    width_button_->setText(width_text(shape_border_width_));
    opacity_button_->setText(opacity_text(shape_alpha_ / 255.0));
  }
}

void MainWindow::set_annotation_border_width(double width) {
  if (auto selected = view_->selected_annotation(); selected) {
    if (!annotation_edits_allowed_ || !has_border_width(selected->kind) || !warn_signature_once()) return;
    const bool recreate = selected->has_bs;
    if (edit_->run(std::make_shared<edit::SetAnnotationBorderWidthCommand>(view_->selected_annotation_page(), selected->id, width))) {
      show_transient(recreate ? tr("다른 프로그램이 만든 주석이라 다시 만들었습니다 (실행 취소 때 원래 두께는 돌아오지 않을 수 있습니다)")
                              : tr("선택한 주석의 두께를 바꿨습니다"));
    }
    return;
  }
  shape_border_width_ = width;
  update_property_buttons();
}

void MainWindow::set_annotation_opacity(std::uint8_t alpha) {
  if (auto selected = view_->selected_annotation(); selected) {
    if (!annotation_edits_allowed_ || !warn_signature_once()) return;
    if (edit_->run(std::make_shared<edit::SetAnnotationOpacityCommand>(view_->selected_annotation_page(), selected->id, alpha))) {
      show_transient(tr("선택한 주석의 불투명도를 바꿨습니다"));
    }
    return;
  }
  shape_alpha_ = alpha;
  update_property_buttons();
}

std::string MainWindow::annotation_author() const {
  // OS 로그인 이름은 개인 정보라 파일에 쓰지 않는다. 작성자 설정 화면(M5)이 생기면 그 값을 쓴다
  return {};
}

void MainWindow::set_annotation_color(engine::Color color) {
  // 주석이 선택돼 있으면 그 주석의 색만 바꾼다 (다음 주석의 기본 색은 그대로). 선택이 없을 때만 기본 색을 바꾼다
  if (auto selected = view_->selected_annotation(); selected) {
    if (!annotation_edits_allowed_ || !warn_signature_once()) return;
    if (edit_->run(std::make_shared<edit::SetAnnotationColorCommand>(view_->selected_annotation_page(), selected->id, color))) {
      show_transient(tr("선택한 주석의 색을 바꿨습니다 (새 주석의 기본 색은 그대로입니다)"));
    }
    return;
  }
  annotation_color_ = color;
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
  AnnotationDialog dialog(tr("메모"), QString(), this);
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

void MainWindow::on_ink_requested(int page, const std::vector<onpdf::Point>& points, onpdf::Rect bbox) {
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
    show_transient(QString());
    return;
  }
  QStringList parts{kind_name(info.kind)};
  if (has_border_width(info.kind)) parts << width_text(info.has_border ? info.border_width : 1.0);
  if (info.opacity < 0.999f) parts << opacity_text(info.opacity);
  if (!info.author.empty()) parts << QString::fromStdString(info.author);
  if (!info.modified.empty()) parts << short_date(info.modified);
  if (!info.contents.empty()) parts << QString::fromStdString(info.contents).left(60);
  statusBar()->showMessage(parts.join(QStringLiteral(" · ")));
}

bool MainWindow::delete_selected_annotation() {
  auto selected = view_->selected_annotation();
  if (!selected || !delete_annotation_action_->isEnabled()) return false;
  if (!warn_signature_once()) return false;
  const int page = view_->selected_annotation_page();
  if (!edit_->run(std::make_shared<edit::RemoveAnnotationCommand>(page, selected->id))) return false;
  view_->clear_annotation_selection();  // 거절되면 선택을 남긴다
  return true;
}

void MainWindow::on_annotation_edit_requested(int page, const engine::AnnotationInfo& info) {
  if (!annotation_edits_allowed_ || info.kind != engine::AnnotationKind::kText) return;
  AnnotationDialog dialog(tr("메모 수정"), QString::fromStdString(info.contents), this);
  if (dialog.exec() != QDialog::Accepted) return;
  const QString text = dialog.text();
  if (text.toStdString() == info.contents) return;
  if (!warn_signature_once()) return;
  edit_->run(std::make_shared<edit::SetAnnotationContentsCommand>(page, info.id, text.toStdString()));
}

// ---------------------------------------------------------------- 병합·분할

namespace {

QString q_filename(const std::filesystem::path& p) { return QString::fromStdString(onpdf::path_to_utf8(p.filename())); }

QString structure_error_text(const onpdf::Error& e) {
  switch (e.code) {
    case ErrorCode::kPasswordRequired: return MainWindow::tr("암호가 필요한 문서가 있습니다: %1").arg(QString::fromStdString(e.message));
    case ErrorCode::kPermissionDenied: return MainWindow::tr("쪽 추출이 허용되지 않은 문서가 있습니다: %1").arg(QString::fromStdString(e.message));
    case ErrorCode::kNotWritable: return MainWindow::tr("파일에 쓸 수 없습니다. 읽기 전용이거나 다른 프로그램이 사용 중입니다.");
    case ErrorCode::kFolderNotWritable: return MainWindow::tr("이 폴더에 새 파일을 만들 수 없습니다. 다른 폴더를 고르세요.");
    case ErrorCode::kSaveVerifyFailed: return MainWindow::tr("만든 파일을 검증하지 못했습니다: %1").arg(QString::fromStdString(e.message));
    default: return QString::fromStdString(e.message);
  }
}

}  // namespace

void MainWindow::show_structure_progress(const QString& title) {
  delete structure_progress_;
  structure_progress_ = new QProgressDialog(title, tr("취소"), 0, 100, this);
  structure_progress_->setObjectName(QStringLiteral("structure_progress"));
  structure_progress_->setWindowTitle(title);
  structure_progress_->setWindowModality(Qt::NonModal);  // 모달 진행 창은 setValue 마다 이벤트를 재진입 처리한다 (인쇄와 같은 이유)
  structure_progress_->setMinimumDuration(0);
  structure_progress_->setAutoClose(false);
  structure_progress_->setAutoReset(false);
  structure_progress_->setRange(0, 0);  // 입력을 읽는 동안은 진행률이 없다 → busy 표시 (리뷰 I9)
  structure_progress_->setLabelText(tr("%1 (준비 중…)").arg(title));
  structure_progress_->show();
  QProgressDialog* progress = structure_progress_;
  connect(structure_job_, &StructureJob::progress, progress, [progress, title](int percent) {
    if (progress->maximum() == 0) {
      progress->setRange(0, 100);
      progress->setLabelText(title);
    }
    progress->setValue(percent);
  }, Qt::QueuedConnection);
  connect(progress, &QProgressDialog::canceled, this, [this] {
    structure_job_->cancel();
    show_transient(tr("작업이 끝나면 결과를 버립니다…"));
  });
}

void MainWindow::on_merge_action() {
  if (!merge_action_->isEnabled()) return;
  const QString start_dir = session_.has_document() ? QFileInfo(session_.path()).absolutePath() : QDir::homePath();
  const QStringList files = QFileDialog::getOpenFileNames(this, tr("병합할 PDF (고른 순서대로 합칩니다)"), start_dir,
                                                          tr("PDF 문서 (*.pdf)"));
  if (files.isEmpty()) return;
  // 파일은 작업 스레드에서 읽는다 (큰 파일을 메인에서 열면 UI 가 멈춘다). 암호가 필요하면 결과로 돌아와 묻고 다시 시도한다
  std::vector<engine::StructureInput> inputs;
  for (const QString& file : files) inputs.push_back({onpdf::path_from_utf8(file.toUtf8().toStdString()), ""});
  const QFileInfo first(files.first());
  const QString suggested = first.absolutePath() + QLatin1Char('/') + first.completeBaseName() + tr("-병합.pdf");
  const QString output = QFileDialog::getSaveFileName(this, tr("병합 결과 저장"), suggested, tr("PDF 문서 (*.pdf)"));
  if (output.isEmpty()) return;
  merge_files(std::move(inputs), output);
}

bool MainWindow::merge_files(std::vector<engine::StructureInput> inputs, const QString& output) {
  if (inputs.empty() || output.isEmpty() || structure_job_->active()) return false;
  const auto out = onpdf::path_from_utf8(output.toUtf8().toStdString());
  for (const auto& in : inputs) {
    std::error_code ec;
    if (std::filesystem::exists(out, ec) && std::filesystem::equivalent(out, in.path, ec)) {
      show_transient(tr("출력 파일이 입력 파일과 같습니다"));
      return false;
    }
  }
  structure_kind_ = StructureKind::kMerge;
  structure_outputs_ = {out};
  structure_folder_ = QFileInfo(output).absolutePath();
  pending_merge_inputs_ = inputs;
  pending_merge_output_ = output;
  show_structure_progress(tr("병합 중…"));
  const bool started = structure_job_->start([inputs = std::move(inputs), out](const engine::ProgressFn& progress) -> StructureJob::Outcome {
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
    hooks.verify = [&](std::size_t, const std::filesystem::path&, const std::filesystem::path& temp) -> onpdf::Result<void> {
      if (auto c = engine::QpdfTools::check(temp); !c) return std::unexpected(c.error());
      auto n = engine::QpdfTools::page_count(temp);
      if (!n) return std::unexpected(n.error());
      if (*n != expected) return onpdf::make_error(ErrorCode::kSaveVerifyFailed, "merged page count mismatch");
      return {};
    };
    return engine::write_files_safely({out}, hooks);
  });
  update_edit_actions();
  return started;
}

void MainWindow::on_split_action() {
  if (!split_action_->isEnabled() || !session_.has_document()) return;
  // 분할은 디스크의 파일을 읽는다: 수정됐으면 먼저 저장할지 묻는다 (저장 안 함이면 디스크 상태로 나눈다)
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
  dialog.setWindowTitle(tr("분할"));
  dialog.setLabelText(tr("몇 쪽마다 나눌까요? (전체 %1쪽)").arg(count));
  dialog.setInputMode(QInputDialog::IntInput);
  dialog.setIntRange(1, std::max(1, count));
  dialog.setIntValue(1);
  if (dialog.exec() != QDialog::Accepted) return;
  const QString dir = QFileDialog::getExistingDirectory(this, tr("분할 파일을 저장할 폴더"), QFileInfo(session_.path()).absolutePath());
  if (dir.isEmpty()) return;
  split_document(dialog.intValue(), dir);
}

bool MainWindow::split_document(int every, const QString& directory) {
  if (!session_.has_document() || every <= 0 || directory.isEmpty() || structure_job_->active()) return false;
  const int count = session_.handle().info().page_count;
  const auto ranges = onpdf::ranges_every(every, count);
  if (ranges.empty()) return false;
  const engine::StructureInput input{onpdf::path_from_utf8(session_.path().toUtf8().toStdString()),
                                     session_.password().toStdString()};  // 암호 문서도 나눌 수 있게 (리뷰 W2)
  const std::string stem = QFileInfo(session_.path()).completeBaseName().toUtf8().toStdString();
  const auto dir = onpdf::path_from_utf8(directory.toUtf8().toStdString());
  std::vector<std::filesystem::path> outs;
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    outs.push_back(dir / onpdf::path_from_utf8(onpdf::output_name(stem + "-%d.pdf", static_cast<int>(i) + 1,
                                                                   static_cast<int>(ranges.size()), ranges[i], count)));
  }
  int existing = 0;
  for (const auto& p : outs) {
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) ++existing;
  }
  if (existing > 0) {  // 폴더 선택 창은 덮어쓰기를 묻지 않으므로 여기서 한 번 묻는다 (리뷰 W3)
    QMessageBox box(QMessageBox::Question, tr("분할"), tr("같은 이름의 파일 %1개를 덮어씁니다. 계속할까요?").arg(existing),
                    QMessageBox::NoButton, this);
    box.setObjectName(QStringLiteral("split_overwrite"));
    QPushButton* go = box.addButton(tr("덮어쓰기"), QMessageBox::AcceptRole);
    QPushButton* cancel = box.addButton(tr("취소"), QMessageBox::RejectRole);
    box.setDefaultButton(cancel);
    box.exec();
    if (box.clickedButton() != go) return false;
  }
  structure_kind_ = StructureKind::kSplit;
  structure_outputs_ = outs;
  structure_folder_ = directory;
  show_structure_progress(tr("분할 중…"));
  const bool started = structure_job_->start([input, ranges, outs](const engine::ProgressFn& progress) -> StructureJob::Outcome {
    engine::SafeWriteHooks hooks;
    hooks.write = [&](const std::vector<std::filesystem::path>& temps) -> onpdf::Result<void> {
      for (std::size_t i = 0; i < temps.size(); ++i) {
        if (auto r = engine::QpdfTools::extract(input, ranges[i], temps[i]); !r) return std::unexpected(r.error());
        progress(static_cast<int>((i + 1) * 100 / temps.size()));
      }
      return {};
    };
    hooks.verify = [&](std::size_t i, const std::filesystem::path&, const std::filesystem::path& temp) -> onpdf::Result<void> {
      auto n = engine::QpdfTools::page_count(temp);
      if (!n) return std::unexpected(n.error());
      if (*n != ranges[i].last - ranges[i].first + 1) return onpdf::make_error(ErrorCode::kSaveVerifyFailed, "split page count mismatch");
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
    // qpdf 작업은 중단할 수 없어 끝까지 돈다. 결과 파일은 지우지 않는다 — 사용자가 덮어쓰기로 고른 기존 파일이나
    // 이전 분할 결과를 지워 버릴 수 있기 때문 (리뷰 C2). 성공했으면 알리기만, 실패했으면 원래 아무것도 없다
    show_transient(outcome ? tr("취소하기 전에 작업이 끝나 파일이 만들어졌습니다") : tr("취소했습니다"));
    return;
  }
  if (!outcome && kind == StructureKind::kMerge && outcome.error().code == ErrorCode::kPasswordRequired &&
      merge_password_attempts_ < 5) {
    // 어느 입력인지는 메시지 앞의 경로로 찾는다 ("<파일>: ..."). 묻고 같은 병합을 다시 시도한다 (리뷰 I4·W1)
    const std::string& message = outcome.error().message;
    for (auto& input : pending_merge_inputs_) {
      const std::string path = onpdf::path_to_utf8(input.path);
      if (message.rfind(path, 0) != 0) continue;
      QInputDialog dialog(this);
      dialog.setObjectName(QStringLiteral("merge_password"));
      dialog.setWindowTitle(tr("암호"));
      dialog.setLabelText(tr("%1 은(는) 암호로 보호되어 있습니다. 암호를 입력하세요.").arg(q_filename(input.path)));
      dialog.setTextEchoMode(QLineEdit::Password);
      if (dialog.exec() != QDialog::Accepted || dialog.textValue().isEmpty()) {
        show_transient(tr("병합을 취소했습니다"));
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
    QMessageBox box(QMessageBox::Warning, kind == StructureKind::kMerge ? tr("병합하지 못했습니다") : tr("분할하지 못했습니다"),
                    tr("%1\n\n만들어진 파일은 없습니다.").arg(structure_error_text(outcome.error())), QMessageBox::Ok, this);
    box.setObjectName(QStringLiteral("structure_failed"));
    box.exec();
    return;
  }
  if (kind == StructureKind::kMerge) {
    show_transient(tr("병합했습니다"));
    open_file(QString::fromStdString(onpdf::path_to_utf8(outputs.front())));
    return;
  }
  QMessageBox box(QMessageBox::Information, tr("분할"), tr("%1개 파일을 저장했습니다.").arg(outcome->size()), QMessageBox::NoButton, this);
  box.setObjectName(QStringLiteral("split_done"));
  QPushButton* open_folder = box.addButton(tr("폴더 열기"), QMessageBox::ActionRole);
  box.addButton(tr("확인"), QMessageBox::AcceptRole);
  box.exec();
  if (box.clickedButton() == open_folder) QDesktopServices::openUrl(QUrl::fromLocalFile(structure_folder_));
}

void MainWindow::on_thumbnail_context_menu(const QPoint& global_pos, const std::vector<int>& pages) {
  if (pages.empty() || !view_->has_document()) return;
  const bool enabled = delete_pages_action_->isEnabled();
  QMenu menu(this);
  menu.setObjectName(QStringLiteral("thumbnail_menu"));
  QAction* del = menu.addAction(tr("쪽 삭제"), this, [this, pages] { delete_pages(pages); });
  QAction* cw = menu.addAction(tr("오른쪽으로 회전"), this, [this, pages] { rotate_pages(pages, 1); });
  QAction* ccw = menu.addAction(tr("왼쪽으로 회전"), this, [this, pages] { rotate_pages(pages, 3); });
  menu.addSeparator();
  const int before = pages.front();
  const int after = pages.back() + 1;
  QAction* ins_before = menu.addAction(tr("파일에서 쪽 삽입 (앞에)..."), this, [this, before] {
    const QString path = QFileDialog::getOpenFileName(this, tr("쪽을 가져올 PDF"), QString(), tr("PDF 문서 (*.pdf)"));
    if (!path.isEmpty()) insert_pages_from(path, before);
  });
  QAction* ins_after = menu.addAction(tr("파일에서 쪽 삽입 (뒤에)..."), this, [this, after] {
    const QString path = QFileDialog::getOpenFileName(this, tr("쪽을 가져올 PDF"), QString(), tr("PDF 문서 (*.pdf)"));
    if (!path.isEmpty()) insert_pages_from(path, after);
  });
  for (QAction* action : {del, cw, ccw, ins_before, ins_after}) action->setEnabled(enabled);
  menu.exec(global_pos);
}

void MainWindow::on_document_changed(const onpdf::engine::ChangeSet& change) {
  pending_insert_.reset();
  if (change.structural) {
    // 쪽 수·순서가 바뀌었다: 뷰·썸네일을 새 쪽 목록으로 다시 세우고(현재 쪽은 새 범위로 클램프), 쪽 번호·목차를 갱신한다
    const int n = session_.handle().info().page_count;
    page_spin_->setRange(1, std::max(1, n));  // 뷰 재구성이 current_page_changed 로 값을 넣기 전에 범위를 넓힌다
    page_total_label_->setText(tr("/ %1 쪽").arg(n));
    view_->rebuild_pages(change.focus_page >= 0 ? change.focus_page : std::max(0, view_->current_page()));
    thumbnail_panel_->reset_pages(std::max(0, view_->current_page()));
    thumbnail_panel_->select_pages(change.selected_pages);  // 옮긴·넣은·되살린 쪽을 다시 선택해 연속 조작이 이어진다
    print_prefs_applied_ = false;  // 인쇄 기본값의 쪽 범위는 이전 쪽 수 기준이었다
    load_outline(false);          // 목차 항목의 쪽 번호가 밀렸다 (탭은 바꾸지 않는다)
  } else {
    view_->invalidate_pages(change.pages, !change.new_sizes.empty());
    thumbnail_panel_->invalidate_pages(change.pages);
  }
  // 검색 결과 사각형은 이전 모양 기준 → 같은 검색어로 다시 찾는다
  if (search_ && search_->active() && !search_bar_->query().trimmed().isEmpty()) {
    reset_search();
    on_search_query(search_bar_->query(), search_bar_->options());
  }
  refresh_status();
}

void MainWindow::on_save() {
  if (!save_action_->isEnabled()) return;
  start_save(session_.path(), [] {});
}

void MainWindow::on_save_as() {
  if (!save_as_action_->isEnabled()) return;
  const QString path = QFileDialog::getSaveFileName(this, tr("다른 이름으로 저장"), session_.path(),
                                                    tr("PDF 문서 (*.pdf)"));
  if (!path.isEmpty()) start_save(path, [] {});
}

bool MainWindow::save_to(const QString& path) { return start_save(path, [] {}); }

bool MainWindow::start_save(const QString& path, std::function<void()> then) {
  if (!session_.has_document() || path.isEmpty()) return false;
  if (saving_ || edit_->busy()) {  // 몇 ms 의 창이지만 조용히 멈추지 않는다 (리뷰 I1)
    show_transient(tr("지금은 저장할 수 없습니다. 잠시 후 다시 시도하세요"));
    return false;
  }
  saving_ = true;
  update_edit_actions();
  show_transient(tr("저장 중…"));
  const QByteArray utf8 = path.toUtf8();
  UiDispatcher* dispatcher = &dispatcher_;
  QPointer<MainWindow> self(this);
  const int generation = document_generation_;  // 문서를 열 때마다 증가
  session_.handle().save(
      onpdf::path_from_utf8(std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()))), {},
      // 워커 스레드: 메인으로 넘기기만
      [dispatcher, self, path, generation, then = std::move(then)](onpdf::Result<engine::SaveResult> result) {
        auto shared = std::make_shared<onpdf::Result<engine::SaveResult>>(std::move(result));
        dispatcher->post_to(self, [path, generation, then, shared](MainWindow* window) {
          window->saving_ = false;
          if (generation != window->document_generation_) return;  // 다른 문서로 바뀌었다
          if (!*shared) {
            window->update_edit_actions();
            QString reason = QString::fromStdString(shared->error().message);
            if (shared->error().code == ErrorCode::kNotWritable) {
              reason = window->tr("파일에 쓸 수 없습니다. 읽기 전용이거나 다른 프로그램이 사용 중입니다.\n"
                                  "다른 이름으로 저장하세요.");
            } else if (shared->error().code == ErrorCode::kFolderNotWritable) {
              reason = window->tr("이 폴더에 새 파일을 만들 수 없습니다.\n"
                                  "다른 폴더에 다른 이름으로 저장하세요.");
            }
            QMessageBox box(QMessageBox::Warning, window->tr("저장하지 못했습니다"),
                            window->tr("%1\n\n원본 파일은 바뀌지 않았습니다.").arg(reason), QMessageBox::Ok, window);
            box.setObjectName(QStringLiteral("save_failed"));
            box.exec();
            return;
          }
          window->session_.set_path(path);
          window->edit_->mark_saved();  // state_changed → 제목·동작 갱신
          window->show_transient((*shared)->incremental ? window->tr("저장했습니다 (서명 보존을 위해 증분 저장)")
                                                       : window->tr("저장했습니다"));
          then();
        });
      });
  return true;
}

void MainWindow::maybe_save(std::function<void()> then) {
  if (!session_.has_document() || !edit_->is_modified()) {
    then();
    return;
  }
  QMessageBox box(QMessageBox::Warning, tr("저장하지 않은 변경"),
                  tr("%1 의 변경 내용을 저장할까요?").arg(QFileInfo(session_.path()).fileName()), QMessageBox::NoButton,
                  this);
  box.setObjectName(QStringLiteral("save_changes_question"));
  box.setInformativeText(tr("저장하지 않으면 변경 내용이 사라집니다."));
  QPushButton* save = box.addButton(tr("저장"), QMessageBox::AcceptRole);
  box.addButton(tr("저장 안 함"), QMessageBox::DestructiveRole);
  QPushButton* cancel = box.addButton(tr("취소"), QMessageBox::RejectRole);
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
  if (structure_job_ != nullptr && structure_job_->active()) {  // 병합·분할이 끝나기 전에는 닫지 않는다 (임시 파일 정리를 위해)
    show_transient(tr("병합·분할이 끝난 뒤 닫을 수 있습니다"));
    event->ignore();
    return;
  }
  if (allow_close_ || !session_.has_document() || !edit_->is_modified()) {
    if (saving_) {  // 저장이 끝날 때까지 닫지 않는다 (원본 교체 중)
      event->ignore();
      return;
    }
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
  const bool has_pages = session_.has_document() && session_.handle().valid() && session_.handle().info().page_count > 0;
  const bool allowed = has_pages && session_.handle().info().print.allowed;
  const bool busy = print_job_ && print_job_->active();
  print_action_->setEnabled(allowed && !busy);
  const QString tip = has_pages && !allowed ? tr("이 문서는 인쇄가 허용되지 않습니다") : tr("문서를 인쇄한다");
  print_action_->setStatusTip(tip);
  print_action_->setToolTip(tip);
  update_edit_actions();  // 인쇄 중에는 편집을 막는다
}

void MainWindow::on_print() {
  if (!print_action_->isEnabled()) return;
  if (!printer_) printer_ = std::make_unique<QPrinter>(QPrinter::HighResolution);
  const auto& info = session_.handle().info();
  if (!print_prefs_applied_) {
    apply_print_preferences(*printer_, info.print, info.page_count);
    print_prefs_applied_ = true;
  }
  QPrintDialog dialog(printer_.get(), this);
  dialog.setWindowTitle(tr("인쇄"));
  dialog.setOptions(QAbstractPrintDialog::PrintPageRange | QAbstractPrintDialog::PrintCurrentPage |
                    QAbstractPrintDialog::PrintCollateCopies | QAbstractPrintDialog::PrintToFile |
                    QAbstractPrintDialog::PrintShowPageSize);
  dialog.setMinMax(1, info.page_count);
  if (dialog.exec() != QDialog::Accepted) return;
  print_with(*printer_);
}

bool MainWindow::print_with(QPrinter& printer) {
  if (!session_.has_document() || !session_.handle().valid()) return false;
  if (print_job_ && print_job_->active()) return false;
  if (edit_->busy()) {  // 편집 결과(쪽 크기)가 반영되기 전의 배치로 인쇄하지 않는다 (리뷰 I6)
    show_transient(tr("편집을 반영하는 중입니다. 잠시 후 다시 인쇄하세요"));
    return false;
  }
  const engine::DocumentHandle handle = session_.handle();
  if (!handle.info().print.allowed) {
    show_transient(tr("이 문서는 인쇄가 허용되지 않습니다"));
    return false;
  }
  PrintJob::Request request = make_print_request(printer, handle.info(), std::max(0, view_->current_page()));
  if (request.steps.empty()) {
    show_transient(tr("인쇄할 쪽이 없습니다"));
    return false;
  }
  print_job_ = std::make_unique<PrintJob>(handle, dispatcher_, printer, std::move(request));
  PrintJob* job = print_job_.get();

  delete print_progress_;
  print_progress_ = new QProgressDialog(tr("인쇄 중…"), tr("취소"), 0, job->total_sheets(), this);
  print_progress_->setWindowTitle(tr("인쇄"));
  // 비모달: 모달 진행 대화상자는 setValue 마다 이벤트를 안에서 처리해 인쇄 처리에 재진입을 일으키고,
  // macOS 에서는 시트 애니메이션이 메인 스레드를 수백 ms 붙잡는다. 문서 교체·창 닫기는 인쇄를 취소한다
  print_progress_->setWindowModality(Qt::NonModal);
  print_progress_->setMinimumDuration(300);
  print_progress_->setAutoClose(false);
  print_progress_->setAutoReset(false);
  auto* bar = new QProgressBar(print_progress_);
  bar->setFormat(tr("%v / %m 장"));  // 레이블을 매 장 바꾸면 대화상자 크기를 다시 계산한다
  bar->setTextVisible(true);
  print_progress_->setBar(bar);
  QProgressDialog* progress = print_progress_;
  // 다음 이벤트 턴에 갱신: 그리기 처리 시간에 UI 갱신이 섞이지 않게
  connect(job, &PrintJob::progress, progress, [progress](int done, int) { progress->setValue(done); },
          Qt::QueuedConnection);
  connect(progress, &QProgressDialog::canceled, this, &MainWindow::cancel_print);
  connect(job, &PrintJob::finished, this, &MainWindow::on_print_finished);
  const bool started = job->start();
  update_print_action();  // 시작한 뒤에야 active 라 인쇄 중 비활성이 반영된다
  return started;
}

bool MainWindow::confirm_cancel_print_for(const QString& path) {
  QMessageBox box(QMessageBox::Question, tr("인쇄 중입니다"),
                  tr("문서를 인쇄하고 있습니다. 인쇄를 취소하고 %1 을(를) 열까요?").arg(QFileInfo(path).fileName()),
                  QMessageBox::NoButton, this);
  box.setObjectName(QStringLiteral("print_busy_question"));
  QPushButton* cancel_and_open = box.addButton(tr("인쇄 취소하고 열기"), QMessageBox::AcceptRole);
  QPushButton* keep = box.addButton(tr("인쇄 계속하기"), QMessageBox::RejectRole);
  box.setDefaultButton(keep);  // 실수로 Enter 를 눌러도 인쇄가 끊기지 않게
  box.exec();
  if (box.clickedButton() != cancel_and_open) return false;
  cancel_print();  // finished(kCancelled) → "인쇄를 취소했습니다"
  return true;
}

void MainWindow::discard_print_job() {
  if (!print_job_) return;
  const bool was_active = print_job_->active();
  print_job_.reset();  // 소멸자가 신호 없이 취소 → on_print_finished 가 오지 않으므로 진행 창은 여기서 닫는다
  if (print_progress_ != nullptr) {
    print_progress_->hide();
    print_progress_->deleteLater();
    print_progress_ = nullptr;
  }
  if (was_active) show_transient(tr("새 문서를 열어 인쇄를 취소했습니다"));
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
      show_transient(tr("%1장 인쇄 완료").arg(job != nullptr ? job->printed_sheets() : 0));
      if (job != nullptr && !job->failed_pages().empty()) {
        QStringList pages;
        for (int page : job->failed_pages()) pages << QString::number(page + 1);
        QMessageBox::warning(this, tr("일부 쪽을 인쇄하지 못했습니다"),
                             tr("다음 쪽은 빈 쪽으로 인쇄되었습니다: %1").arg(pages.join(QStringLiteral(", "))));
      }
      break;
    }
    case PrintJob::Outcome::kCancelled:
      show_transient(tr("인쇄를 취소했습니다"));
      break;
    case PrintJob::Outcome::kFailed:
      QMessageBox::warning(this, tr("인쇄하지 못했습니다"),
                           tr("프린터를 열 수 없거나 문서를 더 읽을 수 없습니다."));
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
            window->show_transient(window->tr("목차를 읽지 못했습니다"));
          }
          window->outline_panel_->set_outline(outline);
          // 문서를 열 때: 목차가 있으면 목차 탭, 없으면 썸네일 탭. 편집 뒤 재로드는 탭을 그대로 둔다
          if (switch_tab) {
            window->sidebar_->setCurrentWidget(outline && !outline->empty()
                                                   ? static_cast<QWidget*>(window->outline_panel_)
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
  if (!view_->has_document() || !sidebar_->isVisible() || sidebar_->currentWidget() != outline_panel_) return;
  if (auto pos = view_->reading_position()) outline_panel_->highlight_for(pos->first, pos->second.y);
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
            [dispatcher, self, on_done = std::move(on_done)](onpdf::Result<view::PageSearchResult> result) {
              auto shared = std::make_shared<onpdf::Result<view::PageSearchResult>>(std::move(result));
              dispatcher->post_to(self, [on_done, shared](MainWindow*) { on_done(std::move(*shared)); });
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
    if (!selected.contains(QLatin1Char('\n')) && selected.size() <= 100) initial = selected.trimmed();
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
    if (search_->wrapped()) show_transient(tr("처음부터 다시 찾습니다"));
  } else if (search_->searching()) {
    show_transient(tr("검색 중…"));
  }
}

void MainWindow::on_find_previous() {
  if (!search_ || !search_->active()) {
    open_find();
    return;
  }
  if (search_->previous()) {
    if (search_->wrapped()) show_transient(tr("끝에서부터 다시 찾습니다"));
  } else if (search_->searching()) {
    show_transient(tr("검색 중…"));
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
  search_bar_->set_status(search_->current_ordinal(), search_->total(), search_->searching(), search_->truncated(),
                          search_->no_text());
}

void MainWindow::on_password_required(const QString& path) {
  if (!screenshot_path_.isEmpty()) {
    fail_screenshot(QStringLiteral("password required"));
    return;
  }
  bool ok = false;
  const QString password = QInputDialog::getText(
      this, tr("암호 필요"), tr("%1 의 암호를 입력하세요:").arg(QFileInfo(path).fileName()),
      QLineEdit::Password, QString(), &ok);
  if (!ok) {
    show_transient(tr("열기 취소"));
    return;
  }
  session_.open(path, password);
}

void MainWindow::on_open_failed(const QString& path, const QString& message) {
  if (!screenshot_path_.isEmpty()) {
    fail_screenshot(message);
    return;
  }
  show_transient(tr("열기 실패: %1").arg(QFileInfo(path).fileName()));
  QMessageBox::warning(this, tr("열 수 없는 파일입니다"),
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
  show_transient(truncated ? tr("%1자 복사됨 (선택이 너무 길어 일부만 복사)").arg(count)
                           : tr("%1자 복사됨").arg(count));
}

void MainWindow::on_select_all() { view_->select_all_on_current_page(); }

void MainWindow::on_link_activated(const onpdf::view::Link& link) {
  if (link.kind == onpdf::view::LinkKind::kPage) {
    navigate_to(link);  // 링크는 location 이 이미 변환되어 있어 바로 이동한다
    return;
  }
  const QString uri = QString::fromUtf8(link.uri.data(), static_cast<qsizetype>(link.uri.size()));
  const QUrl url(uri);
  const bool well_formed = url.isValid() && (url.scheme() == QStringLiteral("mailto") ? !url.path().isEmpty()
                                                                                       : !url.host().isEmpty());
  if (!onpdf::view::is_safe_uri(link.uri) || !well_formed) {
    show_transient(tr("차단된 링크: %1").arg(uri.left(80)));
    return;
  }
  const QString shown = uri.size() > 200 ? uri.left(200) + QStringLiteral("…") : uri;
  const auto answer = QMessageBox::question(this, tr("외부 링크 열기"), tr("외부 링크를 엽니다:\n%1").arg(shown),
                                            QMessageBox::Open | QMessageBox::Cancel, QMessageBox::Cancel);
  if (answer == QMessageBox::Open && !QDesktopServices::openUrl(url)) show_transient(tr("링크를 열지 못했습니다"));
}

void MainWindow::on_link_hovered(const QString& target) {
  link_label_->setText(target.isEmpty() ? QString() : tr("링크: %1").arg(target.left(80)));
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

void MainWindow::on_zoom_changed(double) { refresh_status(); }

void MainWindow::refresh_status() {
  if (!session_.has_document() || !view_->has_document()) return;
  const auto cache = view_->cache_stats();
  const auto sched = view_->scheduler_stats();
  set_document_status(tr("%1 · %2% · 캐시 %3 MB · 타일 %4 ms")
                          .arg(QFileInfo(session_.path()).fileName())
                          .arg(static_cast<int>(view_->zoom() * 100.0 + 0.5))
                          .arg(static_cast<double>(cache.bytes) / (1024.0 * 1024.0), 0, 'f', 1)
                          .arg(sched.avg_render_ms, 0, 'f', 1));
}

void MainWindow::set_document_status(const QString& text) { status_label_->setText(text); }

void MainWindow::show_transient(const QString& text) { statusBar()->showMessage(text, 5000); }

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
  if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event) {
  const QList<QUrl> urls = event->mimeData()->urls();
  if (urls.isEmpty()) return;
  const QString path = urls.first().toLocalFile();
  if (!path.toLower().endsWith(QStringLiteral(".pdf"))) {
    show_transient(tr("PDF 파일만 열 수 있습니다"));
    return;
  }
  event->acceptProposedAction();
  // 열기는 다음 이벤트 턴에: open_file 은 인쇄 중이면 모달 확인을 띄우는데, drop 처리 안에서 중첩 이벤트 루프를
  // 돌리면 끌기 원본(macOS Finder·Windows OLE)이 drop 완료를 기다리며 멈춘다 (#27)
  QTimer::singleShot(0, this, [this, path] { open_file(path); });
}

}  // namespace onpdf::app
