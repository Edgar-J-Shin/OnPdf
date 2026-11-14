#include "main_window.h"

#include "page_view.h"

#include <QAction>
#include <QApplication>
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
#include <QStatusBar>
#include <QTimer>
#include <QUrl>

namespace onpdf::app {

MainWindow::MainWindow(engine::PdfiumWorker& worker, UiDispatcher& dispatcher, QWidget* parent)
    : QMainWindow(parent), session_(worker, dispatcher, this) {
  setWindowTitle(QStringLiteral("OnPdf"));
  setAcceptDrops(true);

  page_view_ = new PageView(this);
  page_view_->clear(tr("파일을 열거나 여기로 끌어다 놓으세요 (Ctrl+O)"));
  setCentralWidget(page_view_);

  status_label_ = new QLabel(this);
  statusBar()->addWidget(status_label_, 1);
  set_status(tr("준비"));

  build_menus();

  connect(&session_, &DocumentSession::opened, this, &MainWindow::on_opened);
  connect(&session_, &DocumentSession::password_required, this,
          &MainWindow::on_password_required);
  connect(&session_, &DocumentSession::open_failed, this, &MainWindow::on_open_failed);
  connect(&session_, &DocumentSession::page_rendered, this, &MainWindow::on_page_rendered);
  connect(&session_, &DocumentSession::render_failed, this, &MainWindow::on_render_failed);
  connect(page_view_, &PageView::viewport_resized, this, &MainWindow::request_render);

  resize(900, 1000);
}

void MainWindow::build_menus() {
  QMenu* file = menuBar()->addMenu(tr("파일(&F)"));
  QAction* open = file->addAction(tr("열기(&O)..."), QKeySequence::Open, this,
                                  &MainWindow::on_open_action);
  open->setStatusTip(tr("PDF 파일을 연다"));
  file->addSeparator();
  file->addAction(tr("종료(&Q)"), QKeySequence::Quit, qApp, &QApplication::quit);

  QMenu* help = menuBar()->addMenu(tr("도움말(&H)"));
  help->addAction(tr("OnPdf 정보(&A)"), this, [this] {
    QMessageBox::about(this, tr("OnPdf 정보"),
                       tr("OnPdf 0.0.1 — PDFium 기반 PDF 뷰어·편집기\nApache License 2.0"));
  });
}

void MainWindow::open_file(const QString& path) {
  set_status(tr("여는 중: %1").arg(QFileInfo(path).fileName()));
  session_.open(path);
}

void MainWindow::screenshot_after_render(const QString& png_path) { screenshot_path_ = png_path; }

void MainWindow::on_open_action() {
  const QString path = QFileDialog::getOpenFileName(this, tr("PDF 열기"), QString(),
                                                    tr("PDF 문서 (*.pdf);;모든 파일 (*)"));
  if (!path.isEmpty()) open_file(path);
}

void MainWindow::on_opened(const QString& path, int page_count) {
  current_page_ = 0;
  setWindowTitle(QStringLiteral("OnPdf — %1").arg(QFileInfo(path).fileName()));
  set_status(tr("%1 · %2 쪽").arg(QFileInfo(path).fileName()).arg(page_count));
  if (page_count <= 0) {
    page_view_->clear(tr("쪽이 없는 문서입니다"));
    return;
  }
  request_render();
}

void MainWindow::on_password_required(const QString& path) {
  bool ok = false;
  const QString password = QInputDialog::getText(
      this, tr("암호 필요"), tr("%1 의 암호를 입력하세요:").arg(QFileInfo(path).fileName()),
      QLineEdit::Password, QString(), &ok);
  if (!ok) {
    set_status(tr("열기 취소"));
    return;
  }
  session_.open(path, password);
}

void MainWindow::on_open_failed(const QString& path, const QString& message) {
  set_status(tr("열기 실패"));
  QMessageBox::warning(this, tr("열 수 없는 파일입니다"),
                       tr("%1\n\n%2").arg(QFileInfo(path).fileName(), message));
}

void MainWindow::on_page_rendered(int page_index, const QImage& image, int elapsed_ms) {
  if (page_index != current_page_ || !session_.has_document()) return;
  const auto& info = session_.info();
  const engine::PageSize size = info.page_sizes[static_cast<std::size_t>(page_index)];
  page_view_->set_page(image, size);
  set_status(tr("%1 · %2 / %3 쪽 · %4×%5 pt · %6 ms")
                 .arg(QFileInfo(session_.path()).fileName())
                 .arg(page_index + 1)
                 .arg(info.page_count)
                 .arg(size.width, 0, 'f', 0)
                 .arg(size.height, 0, 'f', 0)
                 .arg(elapsed_ms));

  if (!screenshot_path_.isEmpty()) {
    const QString path = screenshot_path_;
    screenshot_path_.clear();
    // 이번 paint 가 끝난 뒤 캡처
    QTimer::singleShot(0, this, [this, path] {
      const bool ok = grab().save(path);
      if (!ok) qWarning("screenshot failed: %s", qPrintable(path));
      QApplication::exit(ok ? 0 : 1);
    });
  }
}

void MainWindow::on_render_failed(int page_index, const QString& message) {
  set_status(tr("%1 쪽 렌더 실패: %2").arg(page_index + 1).arg(message));
}

void MainWindow::request_render() {
  if (!session_.has_document()) return;
  const auto& sizes = session_.info().page_sizes;
  if (current_page_ < 0 || static_cast<std::size_t>(current_page_) >= sizes.size()) return;
  const engine::PageSize size = sizes[static_cast<std::size_t>(current_page_)];
  const qreal dpr = page_view_->devicePixelRatioF();
  const float scale = page_view_->fit_width_scale(size) * static_cast<float>(dpr);
  session_.render_page(current_page_, scale, dpr);
}

void MainWindow::set_status(const QString& text) { status_label_->setText(text); }

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
  if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event) {
  const QList<QUrl> urls = event->mimeData()->urls();
  if (urls.isEmpty()) return;
  const QString path = urls.first().toLocalFile();
  if (!path.toLower().endsWith(QStringLiteral(".pdf"))) {
    set_status(tr("PDF 파일만 열 수 있습니다"));
    return;
  }
  event->acceptProposedAction();
  open_file(path);
}

}  // namespace onpdf::app
