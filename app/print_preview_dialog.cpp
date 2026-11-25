#include "print_preview_dialog.h"

#include "qt_image.h"
#include "ui_dispatcher.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPageLayout>
#include <QPaintEvent>
#include <QPainter>
#include <QPointer>
#include <QPrinter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <memory>

namespace onpdf::app {

// ---------------------------------------------------------------- PrintPreviewView

PrintPreviewView::PrintPreviewView(engine::DocumentHandle handle, UiDispatcher& dispatcher,
                                   QWidget* parent)
    : QAbstractScrollArea(parent), handle_(std::move(handle)), dispatcher_(&dispatcher) {
  setObjectName(QStringLiteral("print_preview_view"));
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  viewport()->setBackgroundRole(QPalette::Dark);
  viewport()->setAutoFillBackground(true);
  debounce_.setSingleShot(true);
  debounce_.setInterval(50);
  connect(&debounce_, &QTimer::timeout, this, &PrintPreviewView::request_visible);
}

PrintPreviewView::~PrintPreviewView() {
  cancel_all();
}

void PrintPreviewView::set_paper(engine::PageSize paper_pt, Rect printable_pt) {
  if (!(paper_pt.width > 0.0) || !(paper_pt.height > 0.0)) return;
  paper_pt_ = paper_pt;
  printable_pt_ = printable_pt;
  bump_generation();
  relayout();
}

void PrintPreviewView::set_sheets(std::vector<int> pages) {
  sheets_ = std::move(pages);
  bump_generation();
  relayout();
}

void PrintPreviewView::set_scaling(view::PrintScaling scaling) {
  if (scaling_ == scaling) return;
  scaling_ = scaling;
  viewport()->update();  // 배치만 바뀐다 — 이미지는 배율 모드와 무관한 해상도다
}

void PrintPreviewView::set_grayscale(bool grayscale) {
  if (grayscale_ == grayscale) return;
  grayscale_ = grayscale;
  viewport()->update();
}

int PrintPreviewView::sheet_page(int sheet) const {
  if (sheet < 0 || sheet >= sheet_count()) return -1;
  return sheets_[static_cast<std::size_t>(sheet)];
}

engine::PageSize PrintPreviewView::page_size(int page) const {
  const auto& sizes = handle_.info().page_sizes;
  if (page < 0 || static_cast<std::size_t>(page) >= sizes.size()) return {};
  return sizes[static_cast<std::size_t>(page)];
}

std::optional<view::Placement> PrintPreviewView::placement_of(int sheet) const {
  const int page = sheet_page(sheet);
  if (page < 0) return std::nullopt;
  const engine::PageSize size = page_size(page);
  return view::place_page(size, {printable_pt_.w, printable_pt_.h}, scaling_);
}

void PrintPreviewView::bump_generation() {
  ++generation_;
  cache_.clear();
  failed_.clear();
  cancel_all();
}

void PrintPreviewView::cancel_all() {
  for (auto& [page, pending] : pending_) pending.task.cancel();
  pending_.clear();
}

void PrintPreviewView::relayout() {
  const int width = viewport()->width();
  zoom_ = std::max(0.05, static_cast<double>(width - 2 * kMargin) / paper_pt_.width);
  sheet_h_px_ = paper_pt_.height * zoom_ + kGap;
  const double total = kMargin + sheet_h_px_ * sheet_count();
  verticalScrollBar()->setRange(
      0, std::max(0, static_cast<int>(std::ceil(total)) - viewport()->height()));
  verticalScrollBar()->setPageStep(viewport()->height());
  verticalScrollBar()->setSingleStep(static_cast<int>(sheet_h_px_ / 10.0) + 1);
  // 렌더 배율: 화면 배율 × dpr, 150dpi 상당 상한, 1/64 양자화 — 바뀌면 캐시가 맞지 않는다
  const double raw = std::min(zoom_ * devicePixelRatioF(), kMaxPreviewScale);
  const auto base = static_cast<float>(std::max(1.0, std::round(raw * 64.0)) / 64.0);
  if (base != base_scale_) {
    base_scale_ = base;
    bump_generation();
  }
  viewport()->update();
  schedule_request();
}

QRectF PrintPreviewView::sheet_rect(int sheet) const {
  const double top = kMargin + sheet_h_px_ * sheet - verticalScrollBar()->value();
  return {static_cast<double>(kMargin), top, paper_pt_.width * zoom_, paper_pt_.height * zoom_};
}

std::pair<int, int> PrintPreviewView::visible_sheets() const {
  if (sheet_count() == 0 || sheet_h_px_ <= 0.0) return {0, -1};
  const double scroll = verticalScrollBar()->value();
  const int first =
      std::clamp(static_cast<int>((scroll - kMargin) / sheet_h_px_), 0, sheet_count() - 1);
  const int last =
      std::clamp(static_cast<int>((scroll - kMargin + viewport()->height()) / sheet_h_px_), first,
                 sheet_count() - 1);
  return {first, last};
}

void PrintPreviewView::resizeEvent(QResizeEvent* event) {
  QAbstractScrollArea::resizeEvent(event);
  relayout();
}

void PrintPreviewView::scrollContentsBy(int dx, int dy) {
  QAbstractScrollArea::scrollContentsBy(dx, dy);
  viewport()->update();
  schedule_request();
}

void PrintPreviewView::schedule_request() {
  debounce_.start();
}

void PrintPreviewView::request_visible() {
  if (!handle_.valid() || sheet_count() == 0 || base_scale_ <= 0.0f) return;
  const auto [first, last] = visible_sheets();
  if (last < first) return;
  // 보이는 장 ±1 의 쪽 — 가까운 것부터, 중복 없이
  std::vector<int> wanted;
  const int lo = std::max(0, first - 1), hi = std::min(sheet_count() - 1, last + 1);
  for (int sheet = lo; sheet <= hi; ++sheet) {
    const int page = sheets_[static_cast<std::size_t>(sheet)];
    if (std::find(wanted.begin(), wanted.end(), page) == wanted.end()) wanted.push_back(page);
  }
  // 범위를 벗어난 대기 요청은 취소한다
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (std::find(wanted.begin(), wanted.end(), it->first) == wanted.end()) {
      it->second.task.cancel();
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }
  for (const int page : wanted) {
    if (static_cast<int>(pending_.size()) >= kMaxConcurrent) break;
    if (pending_.contains(page) || failed_.contains(page)) continue;
    const engine::PageSize size = page_size(page);
    const auto placement = view::place_page(size, {printable_pt_.w, printable_pt_.h}, scaling_);
    if (!placement) continue;
    if (const Entry* entry = cache_.object(page);
        entry != nullptr && entry->rotation == placement->rotation)
      continue;
    engine::RenderOptions options;
    options.scale = base_scale_;
    options.rotation = placement->rotation;  // 쪽·용지 방향으로만 정해져 배율 모드와 무관하다
    options.printing = true;                 // 인쇄와 같은 주석 규칙 (/F Print)
    const int generation = generation_;
    const std::uint64_t serial = ++next_serial_;
    UiDispatcher* dispatcher = dispatcher_;
    QPointer<PrintPreviewView> self(this);
    engine::TaskHandle task = handle_.render_page(
        page, options, engine::Priority::kLow,
        // 워커 스레드: 메인으로 넘기기만 (위젯 포인터는 QPointer 로만)
        [dispatcher, self, page, generation, serial](onpdf::Result<onpdf::Bitmap> result) {
          auto shared = std::make_shared<onpdf::Result<onpdf::Bitmap>>(std::move(result));
          dispatcher->post_to(self, [page, generation, serial, shared](PrintPreviewView* view) {
            view->on_rendered(page, generation, serial, std::move(*shared));
          });
        });
    pending_[page] = Pending{std::move(task), serial};
    ++requested_total_;
  }
}

void PrintPreviewView::on_rendered(int page, int generation, std::uint64_t serial,
                                   onpdf::Result<onpdf::Bitmap> result) {
  if (generation != generation_) return;  // 이전 용지·장 목록·배율
  auto it = pending_.find(page);
  if (it == pending_.end() || it->second.serial != serial) return;
  pending_.erase(it);
  if (!result) {
    if (result.error().code != ErrorCode::kCancelled) failed_.insert(page);  // 빈 용지로 둔다
    schedule_request();
    return;
  }
  auto entry = std::make_unique<Entry>();
  entry->image = to_qimage(std::move(*result), devicePixelRatioF());
  const auto placement =
      view::place_page(page_size(page), {printable_pt_.w, printable_pt_.h}, scaling_);
  entry->rotation = placement ? placement->rotation : 0;
  const int cost = std::max(1, static_cast<int>(entry->image.sizeInBytes() / 1024));
  cache_.insert(page, entry.release(), cost);
  viewport()->update();
  schedule_request();
}

void PrintPreviewView::paintEvent(QPaintEvent* event) {
  QPainter painter(viewport());
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  painter.setRenderHint(QPainter::Antialiasing, false);
  const auto [first, last] = visible_sheets();
  for (int sheet = first; sheet <= last; ++sheet) {
    const QRectF paper = sheet_rect(sheet);
    if (!paper.intersects(QRectF(event->rect()))) continue;
    // 용지: 그림자 + 흰 면 + 테두리
    painter.fillRect(paper.translated(2.0, 2.0), QColor(0, 0, 0, 90));
    painter.fillRect(paper, Qt::white);
    painter.setPen(QPen(QColor(120, 120, 120), 1.0));
    painter.drawRect(paper);
    // 인쇄 가능 영역 (점선)
    const QRectF printable(paper.x() + printable_pt_.x * zoom_, paper.y() + printable_pt_.y * zoom_,
                           printable_pt_.w * zoom_, printable_pt_.h * zoom_);
    QPen dashed(QColor(170, 170, 170), 1.0, Qt::DashLine);
    painter.setPen(dashed);
    painter.drawRect(printable);
    const auto placement = placement_of(sheet);
    if (!placement) continue;
    const QRectF target(printable.x() + placement->target.x * zoom_,
                        printable.y() + placement->target.y * zoom_, placement->target.w * zoom_,
                        placement->target.h * zoom_);
    const int page = sheet_page(sheet);
    Entry* entry = cache_.object(page);
    painter.save();
    painter.setClipRect(paper);  // 용지 밖은 그리지 않는다
    if (entry != nullptr && entry->rotation == placement->rotation && !entry->image.isNull()) {
      if (grayscale_) {
        if (entry->gray.isNull())
          entry->gray = entry->image.convertToFormat(QImage::Format_Grayscale8);
        painter.drawImage(target, entry->gray);
      } else {
        painter.drawImage(target, entry->image);
      }
    }
    // 인쇄 가능 영역 밖으로 넘치는 부분(실제 크기에서 잘림)은 회색으로 덮는다
    QRegion outside(target.toAlignedRect());
    outside -= QRegion(printable.toAlignedRect());
    if (!outside.isEmpty()) {
      painter.setClipRegion(outside & QRegion(paper.toAlignedRect()));
      painter.fillRect(target, QColor(0, 0, 0, 70));
    }
    painter.restore();
  }
}

// ---------------------------------------------------------------- PrintPreviewDialog

PrintPreviewDialog::PrintPreviewDialog(engine::DocumentHandle handle, UiDispatcher& dispatcher,
                                       QPrinter& printer, PrintJob::Request request,
                                       QWidget* parent)
    : QDialog(parent), printer_(printer), request_(std::move(request)) {
  setObjectName(QStringLiteral("print_preview_dialog"));
  setWindowTitle(tr("Print Preview"));
  setModal(true);
  resize(720, 820);

  auto* top = new QHBoxLayout;
  scaling_ = new QComboBox(this);
  scaling_->setObjectName(QStringLiteral("print_preview_scaling"));
  scaling_->addItem(tr("Fit to Page"), static_cast<int>(view::PrintScaling::kFit));
  scaling_->addItem(tr("Actual Size"), static_cast<int>(view::PrintScaling::kActualSize));
  scaling_->addItem(tr("Shrink Oversized Pages"),
                    static_cast<int>(view::PrintScaling::kShrinkOversized));
  scaling_->setCurrentIndex(scaling_->findData(static_cast<int>(request_.scaling)));
  grayscale_ = new QCheckBox(tr("Grayscale"), this);
  grayscale_->setObjectName(QStringLiteral("print_preview_grayscale"));
  grayscale_->setChecked(request_.grayscale);
  settings_ = new QPushButton(tr("Settings…"), this);
  settings_->setObjectName(QStringLiteral("print_preview_settings"));
  settings_->setToolTip(tr("Change printer, paper, page range or copies"));
  sheets_label_ = new QLabel(this);
  top->addWidget(new QLabel(tr("Scaling:"), this));
  top->addWidget(scaling_);
  top->addWidget(grayscale_);
  top->addStretch(1);
  top->addWidget(sheets_label_);
  top->addWidget(settings_);

  view_ = new PrintPreviewView(std::move(handle), dispatcher, this);

  auto* buttons = new QDialogButtonBox(this);
  QPushButton* print = buttons->addButton(tr("Print"), QDialogButtonBox::AcceptRole);
  print->setObjectName(QStringLiteral("print_preview_print"));
  print->setDefault(true);
  buttons->addButton(QDialogButtonBox::Cancel);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto* layout = new QVBoxLayout(this);
  layout->addLayout(top);
  layout->addWidget(view_, 1);
  layout->addWidget(buttons);

  connect(scaling_, &QComboBox::currentIndexChanged, this, [this](int) {
    view_->set_scaling(static_cast<view::PrintScaling>(scaling_->currentData().toInt()));
  });
  connect(grayscale_, &QCheckBox::toggled, this, [this](bool on) { view_->set_grayscale(on); });
  connect(settings_, &QPushButton::clicked, this, &PrintPreviewDialog::settings_requested);

  view_->set_scaling(request_.scaling);
  view_->set_grayscale(request_.grayscale);
  apply_paper();
  set_request(request_);
}

void PrintPreviewDialog::apply_paper() {
  const QPageLayout layout = printer_.pageLayout();
  const QRectF full = layout.fullRect(QPageLayout::Point);
  const QRectF paint = layout.paintRect(QPageLayout::Point);
  view_->set_paper({full.width(), full.height()},
                   {paint.x() - full.x(), paint.y() - full.y(), paint.width(), paint.height()});
}

void PrintPreviewDialog::set_request(PrintJob::Request request) {
  request_ = std::move(request);
  std::vector<int> sheets;
  for (const view::PrintStep& step : request_.steps) {
    for (int i = 0; i < std::max(1, step.repeat); ++i) sheets.push_back(step.page);
  }
  apply_paper();  // 설정 재진입으로 용지가 바뀌었을 수 있다
  view_->set_sheets(std::move(sheets));
  update_sheet_label();
}

PrintJob::Request PrintPreviewDialog::request() const {
  PrintJob::Request request = request_;
  request.scaling = static_cast<view::PrintScaling>(scaling_->currentData().toInt());
  request.grayscale = grayscale_->isChecked();
  return request;
}

void PrintPreviewDialog::update_sheet_label() {
  sheets_label_->setText(tr("%n sheet(s)", nullptr, view_->sheet_count()));
}

}  // namespace onpdf::app
