#pragma once

#include "onpdf/core/engine/document_handle.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/view/print_layout.h"

#include <QImage>
#include <QObject>
#include <QPainter>
#include <cstddef>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QPrinter;

namespace onpdf::app {

class UiDispatcher;

// 인쇄 작업 하나. 쪽은 워커에서 래스터로 렌더(kNormal, 동시 최대 2장)하고, 메인 스레드에서 쪽
// 순서대로 QPrinter 에 그린다. 프린터는 작업보다 오래 살아야 한다 (MainWindow 소유).
class PrintJob : public QObject {
  Q_OBJECT
 public:
  enum class Outcome { kCompleted, kCancelled, kFailed };
  struct Request {
    std::vector<view::PrintStep> steps;
    view::PrintScaling scaling = view::PrintScaling::kFit;
    bool grayscale = false;
    bool high_quality = true;
  };

  PrintJob(engine::DocumentHandle handle, UiDispatcher& dispatcher, QPrinter& printer,
           Request request, QObject* parent = nullptr);
  // 진행 중이면 신호 없이 취소한다 (소유자가 소멸 중일 수 있으므로)
  ~PrintJob() override;

  // 인쇄를 시작한다. 시작하지 못하면 false (finished(kFailed) 는 다음 이벤트 턴에)
  bool start();
  // 대기 렌더 취소 + 프린터 작업 중단 → finished(kCancelled) (다음 이벤트 턴). 이미 끝났으면
  // 아무것도 하지 않는다
  void cancel();

  [[nodiscard]] bool active() const noexcept { return active_; }
  [[nodiscard]] int total_sheets() const noexcept { return total_sheets_; }
  [[nodiscard]] int printed_sheets() const noexcept { return printed_sheets_; }
  [[nodiscard]] const std::vector<int>& failed_pages() const noexcept {
    return failed_pages_;
  }  // 0 기준
  // 테스트·측정용
  [[nodiscard]] int max_in_flight() const noexcept { return max_in_flight_; }
  [[nodiscard]] qint64 max_draw_ms() const noexcept { return max_draw_ms_; }
  [[nodiscard]] qint64 max_new_page_ms() const noexcept { return max_new_page_ms_; }
  // 렌더 결과 하나를 처리(그리기·진행 신호 포함)하는 데 메인 스레드가 쓴 최대 시간
  [[nodiscard]] qint64 max_handler_ms() const noexcept { return max_handler_ms_; }
  [[nodiscard]] const Request& request() const noexcept { return request_; }

  static constexpr int kMaxInFlight = 2;  // 렌더 중 + 그리기 대기 비트맵 합

 signals:
  void progress(int printed_sheets, int total_sheets);
  void finished(onpdf::app::PrintJob::Outcome outcome);

 private:
  struct Pending {
    engine::TaskHandle task;
    std::uint64_t serial = 0;
  };
  struct Slot {
    QImage image;  // null 이면 렌더 실패 → 빈 장
    view::Placement placement;
  };

  void pump();                          // request_more + drain 반복
  void request_more();                  // 자리(kMaxInFlight)만큼 렌더 요청
  void request_step(std::size_t step);  // 한 장 렌더 요청 (placements_[step] 이 있어야 한다)
  void drain();                         // 다음 순번부터 준비된 장을 그린다
  void on_rendered(std::size_t step, std::uint64_t serial, onpdf::Result<onpdf::Bitmap> result);
  void finish(Outcome outcome);
  void emit_finished_later(Outcome outcome);

  engine::DocumentHandle handle_;
  UiDispatcher& dispatcher_;
  QPrinter& printer_;
  Request request_;
  QPainter painter_;
  engine::PageSize printable_pt_;
  double device_per_pt_ = 1.0;
  bool active_ = false;
  bool draining_ = false;
  std::size_t next_request_ = 0;
  std::size_t next_draw_ = 0;
  std::uint64_t next_serial_ = 0;
  std::unordered_map<std::size_t, Pending> pending_;
  std::map<std::size_t, Slot> ready_;
  std::unordered_map<std::size_t, view::Placement> placements_;
  std::unordered_set<std::size_t> retried_steps_;  // 렌더 자식 유실 뒤 1회 재요청 (#137)
  int total_sheets_ = 0;
  int printed_sheets_ = 0;
  int max_in_flight_ = 0;
  qint64 max_draw_ms_ = 0;
  qint64 max_new_page_ms_ = 0;
  qint64 max_handler_ms_ = 0;
  std::vector<int> failed_pages_;
};

// 인쇄 대화상자를 거친 QPrinter 상태 + 문서 정보 → 요청. current_page 는 0 기준
[[nodiscard]] PrintJob::Request make_print_request(const QPrinter& printer,
                                                   const engine::DocumentInfo& info,
                                                   int current_page);
// 문서가 정한 인쇄 기본값(매수·쪽 범위·양면)을 대화상자 초기값으로 넣는다. 이전 문서의 범위는
// 지운다
void apply_print_preferences(QPrinter& printer, const view::PrintPreferences& prefs,
                             int page_count);

}  // namespace onpdf::app
