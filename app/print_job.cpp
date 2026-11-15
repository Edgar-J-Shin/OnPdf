#include "print_job.h"

#include "qt_image.h"
#include "ui_dispatcher.h"

#include <QElapsedTimer>
#include <QPageLayout>
#include <QPageRanges>
#include <QPointer>
#include <QPrinter>
#include <QScopedValueRollback>

#include <algorithm>
#include <memory>
#include <utility>

namespace onpdf::app {

PrintJob::PrintJob(engine::DocumentHandle handle, UiDispatcher& dispatcher, QPrinter& printer, Request request,
                   QObject* parent)
    : QObject(parent), handle_(std::move(handle)), dispatcher_(dispatcher), printer_(printer),
      request_(std::move(request)) {
  for (const auto& step : request_.steps) total_sheets_ += std::max(step.repeat, 1);
}

PrintJob::~PrintJob() {
  if (active_) {
    const QSignalBlocker blocker(this);
    finish(Outcome::kCancelled);
  }
}

bool PrintJob::start() {
  if (active_) return false;
  if (!handle_.valid() || request_.steps.empty()) {
    emit_finished_later(Outcome::kFailed);
    return false;
  }
  // 인쇄 가능 영역(포인트). fullPage=false 이므로 painter 원점이 이 영역의 좌상단이다
  const QRectF paint = printer_.pageLayout().paintRect(QPageLayout::Point);
  printable_pt_ = engine::PageSize{paint.width(), paint.height()};
  device_per_pt_ = printer_.resolution() / 72.0;
  if (!painter_.begin(&printer_)) {
    emit_finished_later(Outcome::kFailed);
    return false;
  }
  active_ = true;
  emit progress(0, total_sheets_);
  pump();
  return true;
}

void PrintJob::cancel() {
  if (active_) finish(Outcome::kCancelled);
}

void PrintJob::pump() {
  // 요청 → 그리기를 반복한다. 렌더 없이 준비되는 장(배치 불가)을 그려 자리가 나면 다시 요청해야 멈추지 않는다
  while (active_) {
    request_more();
    const std::size_t drawn_before = next_draw_;
    drain();
    if (next_draw_ == drawn_before) break;
  }
}

void PrintJob::request_more() {
  const auto& sizes = handle_.info().page_sizes;
  while (active_ && next_request_ < request_.steps.size() &&
         static_cast<int>(pending_.size() + ready_.size()) < kMaxInFlight) {
    const std::size_t step = next_request_++;
    const int page = request_.steps[step].page;
    std::optional<view::Placement> placement;
    if (page >= 0 && static_cast<std::size_t>(page) < sizes.size()) {
      placement = view::place_page(sizes[static_cast<std::size_t>(page)], printable_pt_, request_.scaling);
    }
    if (!placement) {
      ready_[step] = Slot{};  // 배치 불가 → 빈 장으로 기록
      continue;
    }
    const int dpi = view::choose_print_dpi(printer_.resolution(), request_.high_quality, placement->target);
    engine::RenderOptions options;
    options.scale = static_cast<float>(placement->scale * dpi / 72.0);
    options.rotation = placement->rotation;
    options.printing = true;
    options.grayscale = request_.grayscale;
    placements_[step] = *placement;

    const std::uint64_t serial = ++next_serial_;
    UiDispatcher* dispatcher = &dispatcher_;
    QPointer<PrintJob> self(this);
    engine::TaskHandle task = handle_.render_page(
        page, options, engine::Priority::kNormal,
        // 워커 스레드: 메인으로 넘기기만
        [dispatcher, self, step, serial](onpdf::Result<onpdf::Bitmap> result) {
          auto shared = std::make_shared<onpdf::Result<onpdf::Bitmap>>(std::move(result));
          dispatcher->post_to(self, [step, serial, shared](PrintJob* job) {
            job->on_rendered(step, serial, std::move(*shared));
          });
        });
    pending_[step] = Pending{std::move(task), serial};
    max_in_flight_ = std::max(max_in_flight_, static_cast<int>(pending_.size() + ready_.size()));
  }
}

void PrintJob::on_rendered(std::size_t step, std::uint64_t serial, onpdf::Result<onpdf::Bitmap> result) {
  if (!active_) return;
  QElapsedTimer handler_timer;
  handler_timer.start();
  struct Measure {
    QElapsedTimer& timer;
    qint64& max_ms;
    ~Measure() { max_ms = std::max(max_ms, timer.elapsed()); }
  } measure{handler_timer, max_handler_ms_};
  auto it = pending_.find(step);
  if (it == pending_.end() || it->second.serial != serial) return;
  pending_.erase(it);
  Slot slot;
  slot.placement = placements_[step];
  placements_.erase(step);
  if (result) {
    slot.image = to_qimage(std::move(*result));
  } else if (result.error().code == ErrorCode::kInvalidState) {
    finish(Outcome::kFailed);  // 문서가 닫혔다
    return;
  }
  ready_[step] = std::move(slot);
  pump();
}

void PrintJob::drain() {
  // 재진입 방지: 그리는 도중 진행 신호 수신자가 이벤트를 처리해 다른 결과가 끼어들어도 순서를 지킨다
  if (draining_) return;
  const QScopedValueRollback guard(draining_, true);
  while (active_) {
    auto it = ready_.find(next_draw_);
    if (it == ready_.end()) break;
    Slot slot = std::move(it->second);
    ready_.erase(it);
    const auto& step = request_.steps[next_draw_];
    if (slot.image.isNull()) failed_pages_.push_back(step.page);
    for (int r = 0; r < std::max(step.repeat, 1); ++r) {
      if (printed_sheets_ > 0) {
        QElapsedTimer page_timer;
        page_timer.start();
        const bool ok = printer_.newPage();  // 인쇄 장치가 이전 장을 마무리(압축·전송)하는 비용 포함
        max_new_page_ms_ = std::max(max_new_page_ms_, page_timer.elapsed());
        if (!ok) {
          finish(Outcome::kFailed);
          return;
        }
      }
      if (!slot.image.isNull()) {
        QElapsedTimer timer;
        timer.start();
        const Rect& t = slot.placement.target;
        painter_.drawImage(QRectF(t.x * device_per_pt_, t.y * device_per_pt_, t.w * device_per_pt_,
                                  t.h * device_per_pt_),
                           slot.image);
        max_draw_ms_ = std::max(max_draw_ms_, timer.elapsed());
      }
      ++printed_sheets_;
      emit progress(printed_sheets_, total_sheets_);
    }
    ++next_draw_;
  }
  if (active_ && next_draw_ >= request_.steps.size()) finish(Outcome::kCompleted);
}

void PrintJob::finish(Outcome outcome) {
  if (!active_) return;
  active_ = false;
  for (auto& [step, pending] : pending_) pending.task.cancel();
  pending_.clear();
  ready_.clear();
  placements_.clear();
  if (outcome != Outcome::kCompleted) printer_.abort();
  if (painter_.isActive()) painter_.end();
  emit_finished_later(outcome);
}

void PrintJob::emit_finished_later(Outcome outcome) {
  // 다음 이벤트 턴에 보낸다: 처리기(모달 대화상자 등)가 drain·on_rendered 호출 스택 안에서 돌지 않게.
  // 그 전에 작업이 파괴되면 이벤트도 함께 버려진다 (파괴한 쪽이 알림을 맡는다)
  QMetaObject::invokeMethod(this, [this, outcome] { emit finished(outcome); }, Qt::QueuedConnection);
}

PrintJob::Request make_print_request(const QPrinter& printer, const engine::DocumentInfo& info, int current_page) {
  PrintJob::Request request;
  const int count = info.page_count;
  std::vector<view::PageRange> ranges;
  switch (printer.printRange()) {
    case QPrinter::CurrentPage:
      if (current_page >= 0 && current_page < count) ranges.push_back({current_page, current_page});
      break;
    case QPrinter::PageRange: {
      for (const auto& r : printer.pageRanges().toRangeList()) ranges.push_back({r.from - 1, r.to - 1});
      if (ranges.empty() && printer.fromPage() > 0) ranges.push_back({printer.fromPage() - 1, printer.toPage() - 1});
      break;
    }
    case QPrinter::AllPages:
    case QPrinter::Selection:
      ranges.push_back({0, count - 1});
      break;
  }
  const int copies = printer.supportsMultipleCopies() ? 1 : std::max(printer.copyCount(), 1);
  request.steps = view::print_sequence(view::pages_in_ranges(ranges, count), copies, printer.collateCopies());
  request.scaling = info.print.no_scaling ? view::PrintScaling::kActualSize : view::PrintScaling::kFit;
  request.grayscale = printer.colorMode() == QPrinter::GrayScale;
  request.high_quality = info.print.high_quality;
  return request;
}

void apply_print_preferences(QPrinter& printer, const view::PrintPreferences& prefs, int page_count) {
  printer.setCopyCount(prefs.copies);
  printer.setPageRanges(QPageRanges());
  printer.setPrintRange(QPrinter::AllPages);
  if (!prefs.page_ranges.empty()) {
    QPageRanges ranges;
    for (const auto& [first, last] : prefs.page_ranges) {
      if (first >= 0 && last < page_count) ranges.addRange(first + 1, last + 1);
    }
    if (!ranges.isEmpty()) {
      printer.setPageRanges(ranges);
      printer.setPrintRange(QPrinter::PageRange);
    }
  }
  switch (prefs.duplex) {
    case view::Duplex::kSimplex: printer.setDuplex(QPrinter::DuplexNone); break;
    case view::Duplex::kFlipShortEdge: printer.setDuplex(QPrinter::DuplexShortSide); break;
    case view::Duplex::kFlipLongEdge: printer.setDuplex(QPrinter::DuplexLongSide); break;
    case view::Duplex::kUndefined: break;
  }
}

}  // namespace onpdf::app
