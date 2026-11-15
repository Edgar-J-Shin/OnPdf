#pragma once

#include "onpdf/core/engine/document_handle.h"
#include "onpdf/core/engine/pdfium_worker.h"

#include <QCache>
#include <QImage>
#include <QWidget>

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

class QListView;
class QModelIndex;
class QTimer;

namespace onpdf::app {

class UiDispatcher;
class ThumbnailModel;

// 쪽 썸네일 세로 목록. 보이는 행 ±4 쪽만 kLow 로 렌더하고(동시 8개), 범위를 벗어난 대기 요청은 취소한다.
// 렌더 결과는 바이트 상한 캐시에 두며, 문서·폭이 바뀌면 세대를 올려 늦게 도착한 결과를 버린다.
class ThumbnailPanel : public QWidget {
  Q_OBJECT
 public:
  explicit ThumbnailPanel(QWidget* parent = nullptr);
  ~ThumbnailPanel() override;

  void set_document(const engine::DocumentHandle& handle, UiDispatcher& dispatcher);
  void clear_document();
  // 현재 쪽 강조 + 목록에서 보이게 스크롤 (page_activated 를 내보내지 않는다)
  void set_current_page(int page);
  [[nodiscard]] int current_page() const noexcept { return current_page_; }

  // 델리게이트용: 쪽 썸네일 (없으면 null QImage)
  [[nodiscard]] QImage thumbnail(int page) const;
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

 protected:
  void resizeEvent(QResizeEvent* event) override;
  void showEvent(QShowEvent* event) override;

 private:
  struct Entry {
    QImage image;
    int generation = 0;
  };
  struct Pending {
    engine::TaskHandle task;
    std::uint64_t serial = 0;
  };

  void schedule_request();
  void request_visible();
  void on_rendered(int page, int generation, std::uint64_t serial, onpdf::Result<onpdf::Bitmap> result);
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
  int generation_ = 0;
  std::uint64_t next_serial_ = 0;
  int requested_total_ = 0;
  int thumb_width_ = 0;
  int current_page_ = -1;
  bool syncing_ = false;  // set_current_page 중에는 사용자 이동으로 보지 않는다
};

}  // namespace onpdf::app
