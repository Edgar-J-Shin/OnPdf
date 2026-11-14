#pragma once

// 문서 하나의 비동기 수명을 관리한다. 위젯을 모른다.
// 모든 메서드와 시그널은 메인 스레드에서만 쓴다. 워커 콜백은 UiDispatcher 를 거쳐 돌아온다.

#include "onpdf/core/engine/document_handle.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "ui_dispatcher.h"

#include <QImage>
#include <QObject>
#include <QString>

#include <cstdint>

namespace onpdf::app {

class DocumentSession : public QObject {
  Q_OBJECT
 public:
  DocumentSession(engine::PdfiumWorker& worker, UiDispatcher& dispatcher,
                  QObject* parent = nullptr);
  ~DocumentSession() override;

  // 열기를 워커에 맡긴다. 이전 열기 요청의 결과는 무시된다. 성공하면 기존 문서를 닫고 교체한다.
  void open(const QString& path, const QString& password = {});
  // page_index 를 scale(포인트당 픽셀) 로 렌더한다. 대기 중인 이전 렌더는 취소된다.
  void render_page(int page_index, float scale, qreal device_pixel_ratio);
  void close();

  [[nodiscard]] bool has_document() const noexcept { return handle_.valid(); }
  [[nodiscard]] const engine::DocumentHandle& handle() const noexcept { return handle_; }
  [[nodiscard]] const engine::DocumentInfo& info() const { return handle_.info(); }
  [[nodiscard]] QString path() const { return path_; }

 signals:
  void opened(const QString& path, int page_count);
  void password_required(const QString& path);
  void open_failed(const QString& path, const QString& message);
  void page_rendered(int page_index, const QImage& image, int elapsed_ms);
  void render_failed(int page_index, const QString& message);

 private:
  engine::PdfiumWorker& worker_;
  UiDispatcher& dispatcher_;
  engine::DocumentHandle handle_;
  QString path_;
  std::uint64_t open_generation_ = 0;
  std::uint64_t render_generation_ = 0;
  engine::TaskHandle pending_open_;
  engine::TaskHandle pending_render_;
};

}  // namespace onpdf::app
