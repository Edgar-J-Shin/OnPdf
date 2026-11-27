#pragma once

// 인쇄 미리보기 (#23). 네이티브 인쇄 대화상자 뒤에 떠서 쪽이 용지에 어떻게 놓이는지(배율·회전·잘림)
// 보여 주고 배율·흑백을 고르게 한다. 배치는 인쇄와 같은 view::place_page 를 쓴다. 쪽 이미지는
// 보이는 장 ±1 만 워커에서 kLow 로 렌더하고(동시 8, 64MB 캐시), 배율 모드와 무관한 고정 해상도라
// 모드를 바꿔도 다시 렌더하지 않는다. QPrintPreviewDialog 는 전 쪽을 동기로 그려 큰 문서에서 UI 가
// 멈춘다.

#include "onpdf/core/engine/document_handle.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/geometry.h"
#include "onpdf/core/view/print_layout.h"

#include "print_job.h"
#include <QAbstractScrollArea>
#include <QCache>
#include <QDialog>
#include <QImage>
#include <QTimer>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QPrinter;
class QPushButton;

namespace onpdf::app {

class UiDispatcher;

// 세로로 이어진 장 목록. 장 = 인쇄될 한 면 (같은 쪽이 매수만큼 반복될 수 있다)
class PrintPreviewView : public QAbstractScrollArea {
  Q_OBJECT
 public:
  PrintPreviewView(engine::DocumentHandle handle, UiDispatcher& dispatcher,
                   QWidget* parent = nullptr);
  ~PrintPreviewView() override;

  // 용지 전체 크기와 인쇄 가능 영역(용지 좌상단 기준, 포인트). 바뀌면 세대를 올린다
  void set_paper(engine::PageSize paper_pt, Rect printable_pt);
  // 장 목록(쪽 번호). 바뀌면 세대를 올려 늦은 렌더를 버린다
  void set_sheets(std::vector<int> pages);
  void set_scaling(view::PrintScaling scaling);  // 배치만 다시 계산한다 (재렌더 없음)
  void set_grayscale(bool grayscale);

  [[nodiscard]] int sheet_count() const noexcept { return static_cast<int>(sheets_.size()); }
  [[nodiscard]] int sheet_page(int sheet) const;
  [[nodiscard]] std::optional<view::Placement> placement_of(int sheet) const;
  // 테스트용
  [[nodiscard]] int requested_total() const noexcept { return requested_total_; }
  [[nodiscard]] int pending_requests() const noexcept { return static_cast<int>(pending_.size()); }
  [[nodiscard]] int cached_count() const { return static_cast<int>(cache_.count()); }
  [[nodiscard]] int generation() const noexcept { return generation_; }
  // 디바운스 대기도 없고 렌더 요청도 없다 (테스트가 기다리는 조건)
  [[nodiscard]] bool idle() const { return !debounce_.isActive() && pending_.empty(); }
  [[nodiscard]] double zoom() const noexcept { return zoom_; }
  void request_visible_now() { request_visible(); }

  static constexpr int kMaxConcurrent = 8;
  static constexpr int kCacheBytes = 64 * 1024 * 1024;
  static constexpr double kMaxPreviewScale = 150.0 / 72.0;  // 쪽 포인트당 픽셀 상한
  static constexpr int kMargin = 16;                        // 논리 픽셀
  static constexpr int kGap = 16;
  static constexpr double kRescaleThreshold =
      0.12;  // 이보다 작은 배율 변화는 옛 이미지를 늘려 쓴다

 protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void scrollContentsBy(int dx, int dy) override;

 private:
  struct Entry {
    QImage image;
    QImage gray;  // 흑백 미리보기용, 처음 필요할 때 만든다
    int rotation = 0;
  };
  struct Pending {
    engine::TaskHandle task;
    std::uint64_t serial = 0;
  };

  void relayout();  // 확대·장 위치·스크롤 범위. 렌더 배율이 바뀌면 세대↑
  void bump_generation();
  void apply_wanted_scale();  // wanted_scale_ 가 base_scale_ 와 12% 넘게 다르면 세대↑
  void cancel_all();
  void schedule_request();
  void request_visible();
  void on_rendered(int page, int generation, std::uint64_t serial,
                   onpdf::Result<onpdf::Bitmap> result);
  [[nodiscard]] QRectF sheet_rect(int sheet) const;          // 뷰포트 좌표(논리 픽셀), 용지 사각형
  [[nodiscard]] std::pair<int, int> visible_sheets() const;  // [first, last], 없으면 {0, -1}
  [[nodiscard]] engine::PageSize page_size(int page) const;

  engine::DocumentHandle handle_;
  UiDispatcher* dispatcher_;
  std::vector<int> sheets_;
  engine::PageSize paper_pt_{612.0, 792.0};
  Rect printable_pt_{0.0, 0.0, 612.0, 792.0};
  view::PrintScaling scaling_ = view::PrintScaling::kFit;
  bool grayscale_ = false;
  double zoom_ = 1.0;        // 용지 포인트 → 논리 픽셀
  float base_scale_ = 0.0f;  // 렌더 배율 (쪽 포인트당 픽셀, 1/64 양자화)
  float wanted_scale_ =
      0.0f;  // 창 크기 변경이 요구하는 배율 — 디바운스 뒤 12% 넘게 다르면 적용 (#101)
  double sheet_h_px_ = 0.0;  // 용지 높이(논리 픽셀) + 간격
  int generation_ = 0;
  QCache<int, Entry> cache_{kCacheBytes / 1024};
  std::unordered_map<int, Pending> pending_;
  std::unordered_set<int> failed_;
  std::unordered_set<int> refused_;  // 렌더 거부 쪽 (#121): 용지에 "표시 불가" 안내
  std::uint64_t next_serial_ = 0;
  int requested_total_ = 0;
  QTimer debounce_;
};

class PrintPreviewDialog : public QDialog {
  Q_OBJECT
 public:
  PrintPreviewDialog(engine::DocumentHandle handle, UiDispatcher& dispatcher, QPrinter& printer,
                     PrintJob::Request request, QWidget* parent = nullptr);

  // 선택한 배율·흑백이 반영된 요청
  [[nodiscard]] PrintJob::Request request() const;
  // 용지·범위·매수가 바뀌었다(설정 재진입): 장 목록을 다시 만든다. 배율·흑백 선택은 유지
  void set_request(PrintJob::Request request);

  [[nodiscard]] PrintPreviewView* view() const noexcept { return view_; }
  [[nodiscard]] QComboBox* scaling_combo() const noexcept { return scaling_; }
  [[nodiscard]] QCheckBox* grayscale_check() const noexcept { return grayscale_; }
  [[nodiscard]] QPushButton* settings_button() const noexcept { return settings_; }

 signals:
  void settings_requested();  // "설정…": 소유자가 인쇄 대화상자를 다시 열고 set_request 로 갱신

 private:
  void apply_paper();
  void update_sheet_label();

  QPrinter& printer_;
  PrintJob::Request request_;
  PrintPreviewView* view_ = nullptr;
  QComboBox* scaling_ = nullptr;
  QCheckBox* grayscale_ = nullptr;
  QPushButton* settings_ = nullptr;
  QLabel* sheets_label_ = nullptr;
};

}  // namespace onpdf::app
