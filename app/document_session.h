#pragma once

// 문서 하나의 비동기 수명을 관리한다. 위젯을 모른다.
// 모든 메서드와 시그널은 메인 스레드에서만 쓴다. 워커 콜백은 UiDispatcher 를 거쳐 돌아온다.

#include "onpdf/core/engine/document_handle.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/engine/render_process.h"

#include "ui_dispatcher.h"
#include <QImage>
#include <QObject>
#include <QString>
#include <cstdint>
#include <memory>

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
  // 다른 이름으로 저장한 뒤: 열린 문서는 그대로, 경로만 바꾼다
  void set_path(const QString& path) { path_ = path; }
  // 렌더 자식 프로세스 (#137): 있으면 열기 성공 때 핸들에 붙여 타일·썸네일·인쇄 렌더가 그쪽으로
  // 간다. nullptr 이면 워커 렌더. 이벤트는 메인 스레드 신호로
  void set_render_process(std::shared_ptr<engine::RenderProcess> process);
  [[nodiscard]] const std::shared_ptr<engine::RenderProcess>& render_process() const noexcept {
    return render_process_;
  }

  [[nodiscard]] bool has_document() const noexcept { return handle_.valid(); }
  [[nodiscard]] const engine::DocumentHandle& handle() const noexcept { return handle_; }
  [[nodiscard]] const engine::DocumentInfo& info() const { return handle_.info(); }
  [[nodiscard]] QString path() const { return path_; }
  [[nodiscard]] QString password() const {
    return password_;
  }  // 열 때 쓴 암호 (분할 등 디스크 파일을 다시 읽는 작업용)

 signals:
  void opened(const QString& path, int page_count);
  void password_required(const QString& path);
  void open_failed(const QString& path, const QString& message);
  void page_rendered(int page_index, const QImage& image, int elapsed_ms);
  void render_failed(int page_index, const QString& message);
  void render_process_restarted(int page, const QString& reason);  // (#137) 자식 재시작
  void render_process_unavailable(const QString& reason);  // (#137) 띄울 수 없음 → 워커 폴백

 private:
  engine::PdfiumWorker& worker_;
  UiDispatcher& dispatcher_;
  engine::DocumentHandle handle_;
  QString path_;
  QString password_;
  std::uint64_t open_generation_ = 0;
  std::uint64_t render_generation_ = 0;
  engine::TaskHandle pending_open_;
  engine::TaskHandle pending_render_;
  std::shared_ptr<engine::RenderProcess> render_process_;
};

}  // namespace onpdf::app
