#pragma once

#include "onpdf/core/engine/document_handle.h"
#include "onpdf/core/engine/pdfium_worker.h"

#include <QCache>
#include <QImage>
#include <QString>
#include <QWidget>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QListView;
class QModelIndex;
class QPoint;
class QTimer;

namespace onpdf::app {

class UiDispatcher;
class ThumbnailModel;

// 쪽 썸네일 세로 목록. 보이는 행 ±4 쪽만 kLow 로 렌더하고(동시 8개), 범위를 벗어난 대기 요청은
// 취소한다. 렌더 결과는 바이트 상한 캐시에 두며, 문서·폭이 바뀌면 세대를 올려 늦게 도착한 결과를
// 버린다.
class ThumbnailPanel : public QWidget {
  Q_OBJECT
 public:
  explicit ThumbnailPanel(QWidget* parent = nullptr);
  ~ThumbnailPanel() override;

  void set_document(const engine::DocumentHandle& handle, UiDispatcher& dispatcher);
  void clear_document();
  // 편집으로 쪽 모양이 바뀌었다: 그 쪽 썸네일을 버리고 다시 렌더한다 (행 높이도 새 쪽 크기로)
  void invalidate_pages(const std::vector<int>& pages);
  // 쪽 수·순서가 바뀌었다(삭제·이동·삽입): 세대를 올려 늦은 렌더를 버리고, 캐시를 비우고, 행 수를
  // 문서 정보에 맞춘 뒤 스크롤 위치와 현재 쪽을 되살린다
  void reset_pages(int current_page);
  // 편집 가능 상태(작업 중 아님·권한 있음)에서만 끌어 놓기·Delete·드롭을 받는다
  void set_editable(bool editable);
  [[nodiscard]] bool editable() const noexcept { return editable_; }
  [[nodiscard]] std::vector<int> selected_pages() const;  // 오름차순
  void select_pages(const std::vector<int>& pages);       // 선택을 이 쪽들로 바꾼다 (비면 그대로)
  [[nodiscard]] bool list_has_focus() const;
  // 내부용(모델·목록이 부른다): 요청을 신호로 바꾼다. 문서는 Command 가 바꾸고, 행은
  // document_changed 뒤에 갱신한다
  void request_move(const std::vector<int>& pages, int drop_row);
  void request_insert_file(const QString& path, int drop_row);
  void request_delete(const std::vector<int>& pages);
  void request_context_menu(const QPoint& global_pos, int row);
  // 현재 쪽 강조 + 목록에서 보이게 스크롤 (page_activated 를 내보내지 않는다)
  void set_current_page(int page);
  [[nodiscard]] int current_page() const noexcept { return current_page_; }

  // 델리게이트용: 쪽 썸네일 (없으면 null QImage)
  [[nodiscard]] QImage thumbnail(int page) const;
  // 렌더가 거부된 쪽(kRenderRefused, #121): 썸네일 대신 "표시 불가" 안내
  [[nodiscard]] bool is_refused(int page) const { return refused_.contains(page); }
  [[nodiscard]] int thumb_width() const noexcept { return thumb_width_; }
  [[nodiscard]] QSize thumb_size(int page) const;  // 논리 픽셀

  // 테스트용
  [[nodiscard]] int pending_requests() const noexcept { return static_cast<int>(pending_.size()); }
  [[nodiscard]] int requested_total() const noexcept { return requested_total_; }
  [[nodiscard]] int cached_count() const { return static_cast<int>(cache_.count()); }
  [[nodiscard]] int generation() const noexcept { return generation_; }
  [[nodiscard]] QListView* list() const noexcept { return list_; }
  void request_visible_now() { request_visible(); }

  static constexpr int kMaxConcurrent = 8;
  static constexpr int kPrefetchRows = 4;
  static constexpr int kCacheBytes = 64 * 1024 * 1024;
  static constexpr int kWidthStep = 32;  // 이 이상 폭이 바뀌어야 다시 렌더

 signals:
  void page_activated(int page);
  // drop_row: 현재 행 기준 삽입 자리 (0..rowCount). MainWindow 가 Command 의 dest/at 로 바꾼다
  void move_requested(const std::vector<int>& pages, int drop_row);
  void insert_file_requested(const QString& path, int drop_row);
  void delete_requested(const std::vector<int>& pages);
  void context_menu_requested(const QPoint& global_pos, const std::vector<int>& pages);

 protected:
  void resizeEvent(QResizeEvent* event) override;
  void showEvent(QShowEvent* event) override;
  bool event(QEvent* event) override;  // DevicePixelRatioChange (모니터 이동)

 private:
  struct Entry {
    QImage image;
    int generation = 0;
    qreal dpr = 1.0;  // 렌더할 때의 화면 배율. 다르면 다시 렌더
  };
  struct Pending {
    engine::TaskHandle task;
    std::uint64_t serial = 0;
  };

  void schedule_request();
  void request_visible();
  void on_rendered(int page, int generation, std::uint64_t serial,
                   onpdf::Result<onpdf::Bitmap> result);
  void update_thumb_width(bool force);
  void cancel_all();
  void on_user_activated(const QModelIndex& index);

  engine::DocumentHandle handle_;
  UiDispatcher* dispatcher_ = nullptr;
  ThumbnailModel* model_ = nullptr;
  QListView* list_ = nullptr;
  QTimer* request_timer_ = nullptr;
  QCache<int, Entry> cache_;
  std::unordered_map<int, Pending> pending_;
  std::unordered_set<int> failed_;
  std::unordered_set<int> retried_;  // 렌더 자식 유실 뒤 1회 재요청한 쪽 (#137)
  std::unordered_set<int> refused_;  // 렌더 거부 쪽 (#121), failed_ 와 같이 비운다
  int generation_ = 0;
  std::uint64_t next_serial_ = 0;
  int requested_total_ = 0;
  int thumb_width_ = 0;
  int current_page_ = -1;
  bool syncing_ = false;  // set_current_page 중에는 사용자 이동으로 보지 않는다
  bool editable_ = false;
};

}  // namespace onpdf::app
