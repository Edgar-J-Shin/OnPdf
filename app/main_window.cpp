#include "main_window.h"

#include "document_view.h"
#include "onpdf/core/platform/path.h"
#include "onpdf/edit/rotate_page.h"
#include "outline_panel.h"
#include "search_bar.h"
#include "thumbnail_panel.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPointer>
#include <QPrintDialog>
#include <QPushButton>
#include <QPrinter>
#include <QProgressBar>
#include <QProgressDialog>
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
  build_menus();
  connect(edit_, &EditController::document_changed, this, &MainWindow::on_document_changed);
  connect(edit_, &EditController::state_changed, this, [this] {
    update_edit_actions();
    update_title();
  });
  connect(edit_, &EditController::failed, this,
          [this](const QString& message) { show_transient(tr("편집하지 못했습니다: %1").arg(message)); });

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
  connect(view_, &DocumentView::selection_changed, this, [this](bool has) { copy_action_->setEnabled(has); });
  connect(view_, &DocumentView::link_activated, this, &MainWindow::on_link_activated);
  connect(view_, &DocumentView::link_hovered, this, &MainWindow::on_link_hovered);
  connect(search_bar_, &SearchBar::query_changed, this, &MainWindow::on_search_query);
  connect(search_bar_, &SearchBar::next_requested, this, &MainWindow::on_find_next);
  connect(search_bar_, &SearchBar::previous_requested, this, &MainWindow::on_find_previous);
  connect(search_bar_, &SearchBar::closed, this, &MainWindow::on_search_closed);
  connect(thumbnail_panel_, &ThumbnailPanel::page_activated, this, [this](int page) {
    if (view_->has_document()) view_->go_to_page(page);
  });
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
  load_outline();
  reset_search();
  // 검색 바가 열려 있으면 새 문서에서 같은 검색어로 다시 찾는다
  if (search_bar_->isVisible() && !search_bar_->query().trimmed().isEmpty()) {
    on_search_query(search_bar_->query(), search_bar_->options());
  }
  refresh_status();
}

// ---------------------------------------------------------------- 편집·저장

QString MainWindow::command_label(std::string_view id) const {
  if (id == "rotate_page") return tr("쪽 회전");
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

void MainWindow::rotate_current_page(int quarter_turns) {
  if (!rotate_cw_action_->isEnabled() || !view_->has_document()) return;
  const int page = std::max(0, view_->current_page());
  edit_->run(std::make_shared<edit::RotatePageCommand>(page, quarter_turns));
}

void MainWindow::on_document_changed(const onpdf::engine::ChangeSet& change) {
  view_->invalidate_pages(change.pages, !change.new_sizes.empty());
  thumbnail_panel_->invalidate_pages(change.pages);
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
  if (saving_ || edit_->busy() || !session_.has_document() || path.isEmpty()) return false;
  saving_ = true;
  update_edit_actions();
  show_transient(tr("저장 중…"));
  const QByteArray utf8 = path.toUtf8();
  UiDispatcher* dispatcher = &dispatcher_;
  QPointer<MainWindow> self(this);
  const int generation = outline_generation_;  // 문서를 열 때마다 증가
  session_.handle().save(
      onpdf::path_from_utf8(std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()))), {},
      // 워커 스레드: 메인으로 넘기기만
      [dispatcher, self, path, generation, then = std::move(then)](onpdf::Result<engine::SaveResult> result) {
        auto shared = std::make_shared<onpdf::Result<engine::SaveResult>>(std::move(result));
        dispatcher->post_to(self, [path, generation, then, shared](MainWindow* window) {
          window->saving_ = false;
          if (generation != window->outline_generation_) return;  // 다른 문서로 바뀌었다
          if (!*shared) {
            window->update_edit_actions();
            const QString reason =
                shared->error().code == ErrorCode::kNotWritable
                    ? window->tr("파일에 쓸 수 없습니다. 읽기 전용이거나 다른 프로그램이 사용 중입니다.\n"
                                 "다른 이름으로 저장하세요.")
                    : QString::fromStdString(shared->error().message);
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

void MainWindow::load_outline() {
  outline_task_.cancel();  // 이전 문서의 목차 순회가 아직 대기 중이면 워커를 점유하지 않게
  const int generation = ++outline_generation_;
  outline_panel_->set_outline(nullptr);
  sidebar_->setCurrentWidget(thumbnail_panel_);
  UiDispatcher* dispatcher = &dispatcher_;
  QPointer<MainWindow> self(this);
  outline_task_ = session_.handle().load_outline(
      engine::Priority::kNormal,
      // 워커 스레드: 메인으로 넘기기만
      [dispatcher, self, generation](onpdf::Result<view::Outline> result) {
        auto shared = std::make_shared<onpdf::Result<view::Outline>>(std::move(result));
        dispatcher->post_to(self, [generation, shared](MainWindow* window) {
          if (generation != window->outline_generation_) return;  // 그 사이 다른 문서를 열었다
          std::shared_ptr<const view::Outline> outline;
          if (*shared) {
            outline = std::make_shared<const view::Outline>(std::move(**shared));
          } else if (shared->error().code != ErrorCode::kCancelled) {
            window->show_transient(window->tr("목차를 읽지 못했습니다"));
          }
          window->outline_panel_->set_outline(outline);
          // 목차가 있으면 목차 탭, 없으면 썸네일 탭
          window->sidebar_->setCurrentWidget(outline && !outline->empty()
                                                 ? static_cast<QWidget*>(window->outline_panel_)
                                                 : static_cast<QWidget*>(window->thumbnail_panel_));
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
  const int generation = outline_generation_;
  const int page = target.page;
  UiDispatcher* dispatcher = &dispatcher_;
  QPointer<MainWindow> self(this);
  session_.handle().resolve_location(
      page, *target.user_location, engine::Priority::kHigh,
      [dispatcher, self, generation, page](onpdf::Result<onpdf::Point> result) {
        auto shared = std::make_shared<onpdf::Result<onpdf::Point>>(std::move(result));
        dispatcher->post_to(self, [generation, page, shared](MainWindow* window) {
          // 그 사이 다른 문서를 열었으면 버린다 (outline_generation_ 은 문서를 열 때마다 증가)
          if (generation != window->outline_generation_ || !window->view_->has_document()) return;
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
