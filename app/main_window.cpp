#include "main_window.h"

#include "document_view.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
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
#include <QSpinBox>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>

namespace onpdf::app {

MainWindow::MainWindow(engine::PdfiumWorker& worker, UiDispatcher& dispatcher, QWidget* parent)
    : QMainWindow(parent), dispatcher_(dispatcher), session_(worker, dispatcher, this) {
  setWindowTitle(QStringLiteral("OnPdf"));
  setAcceptDrops(true);

  view_ = new DocumentView(this);
  view_->clear_document(tr("파일을 열거나 여기로 끌어다 놓으세요 (Ctrl+O)"));
  setCentralWidget(view_);

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

  build_menus();

  connect(&session_, &DocumentSession::opened, this, &MainWindow::on_opened);
  connect(&session_, &DocumentSession::password_required, this, &MainWindow::on_password_required);
  connect(&session_, &DocumentSession::open_failed, this, &MainWindow::on_open_failed);
  connect(view_, &DocumentView::current_page_changed, this, &MainWindow::on_current_page_changed);
  connect(view_, &DocumentView::zoom_changed, this, &MainWindow::on_zoom_changed);
  connect(view_, &DocumentView::first_view_rendered, this, &MainWindow::on_first_view_rendered);
  connect(view_, &DocumentView::selection_changed, this, [this](bool has) { copy_action_->setEnabled(has); });
  connect(view_, &DocumentView::link_activated, this, &MainWindow::on_link_activated);
  connect(view_, &DocumentView::link_hovered, this, &MainWindow::on_link_hovered);
  connect(page_spin_, &QSpinBox::valueChanged, this, [this](int value) {
    if (view_->has_document() && view_->current_page() != value - 1) view_->go_to_page(value - 1);
  });

  resize(900, 1000);
}

void MainWindow::build_menus() {
  QMenu* file = menuBar()->addMenu(tr("파일(&F)"));
  QAction* open = file->addAction(tr("열기(&O)..."), QKeySequence::Open, this, &MainWindow::on_open_action);
  open->setStatusTip(tr("PDF 파일을 연다"));
  file->addSeparator();
  // QKeySequence::Quit 은 Windows 에서 비어 있으므로 Ctrl+Q 를 명시한다
  file->addAction(tr("종료(&Q)"), QKeySequence(Qt::CTRL | Qt::Key_Q), qApp, &QApplication::quit);

  QMenu* edit = menuBar()->addMenu(tr("편집(&E)"));
  copy_action_ = edit->addAction(tr("복사(&C)"), QKeySequence::Copy, this, &MainWindow::on_copy);
  copy_action_->setEnabled(false);
  select_all_action_ = edit->addAction(tr("현재 쪽 전체 선택(&A)"), QKeySequence::SelectAll, this,
                                       &MainWindow::on_select_all);

  QMenu* view = menuBar()->addMenu(tr("보기(&V)"));
  view->addAction(tr("너비 맞춤(&W)"), QKeySequence(Qt::CTRL | Qt::Key_1), this,
                  [this] { view_->set_zoom_mode(ZoomMode::kFitWidth); });
  view->addAction(tr("쪽 맞춤(&P)"), QKeySequence(Qt::CTRL | Qt::Key_2), this,
                  [this] { view_->set_zoom_mode(ZoomMode::kFitPage); });
  view->addAction(tr("실제 크기(&A)"), QKeySequence(Qt::CTRL | Qt::Key_0), this, [this] { view_->set_zoom(1.0); });
  view->addSeparator();
  view->addAction(tr("확대(&I)"), QKeySequence::ZoomIn, this, [this] { view_->zoom_in(); });
  view->addAction(tr("축소(&O)"), QKeySequence::ZoomOut, this, [this] { view_->zoom_out(); });

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
  show_transient(tr("여는 중: %1").arg(QFileInfo(path).fileName()));
  session_.open(path);
}

void MainWindow::on_open_action() {
  const QString path = QFileDialog::getOpenFileName(this, tr("PDF 열기"), QString(),
                                                    tr("PDF 문서 (*.pdf);;모든 파일 (*)"));
  if (!path.isEmpty()) open_file(path);
}

void MainWindow::on_opened(const QString& path, int page_count) {
  setWindowTitle(QStringLiteral("OnPdf — %1").arg(QFileInfo(path).fileName()));
  statusBar()->clearMessage();  // "여는 중" 임시 메시지가 문서 정보를 가리지 않게
  if (page_count <= 0) {
    if (!screenshot_path_.isEmpty()) {
      fail_screenshot(QStringLiteral("no pages"));
      return;
    }
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
  refresh_status();
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
  const QString text = view_->selected_text();
  if (text.isEmpty()) return;
  QGuiApplication::clipboard()->setText(text);
  show_transient(tr("%1자 복사됨").arg(text.size()));
}

void MainWindow::on_select_all() { view_->select_all_on_current_page(); }

void MainWindow::on_link_activated(const onpdf::view::Link& link) {
  if (link.kind == onpdf::view::LinkKind::kPage) {
    if (link.location) {
      view_->scroll_to_page_point(link.page, *link.location);
    } else {
      view_->go_to_page(link.page);
    }
    return;
  }
  const QString uri = QString::fromStdString(link.uri);
  if (!onpdf::view::is_safe_uri(link.uri)) {
    show_transient(tr("차단된 링크: %1").arg(uri.left(80)));
    return;
  }
  const auto answer = QMessageBox::question(this, tr("외부 링크 열기"), tr("외부 링크를 엽니다:\n%1").arg(uri),
                                            QMessageBox::Open | QMessageBox::Cancel, QMessageBox::Cancel);
  if (answer == QMessageBox::Open) QDesktopServices::openUrl(QUrl(uri));
}

void MainWindow::on_link_hovered(const QString& target) {
  link_label_->setText(target.isEmpty() ? QString() : tr("링크: %1").arg(target.left(80)));
}

void MainWindow::on_current_page_changed(int page) {
  if (page >= 0 && page_spin_->value() != page + 1) {
    const QSignalBlocker blocker(page_spin_);
    page_spin_->setValue(page + 1);
  }
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
  open_file(path);
}

}  // namespace onpdf::app
