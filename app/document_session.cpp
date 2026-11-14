#include "document_session.h"

#include "onpdf/core/engine/render_types.h"
#include "qt_image.h"

#include <chrono>
#include <memory>
#include <string>
#include <utility>

namespace onpdf::app {

namespace {

QString error_text(const onpdf::Error& e) {
  // core 메시지가 이미 코드 설명으로 시작하면("file not found: ...") 접두어를 겹치지 않는다
  const std::string code = onpdf::to_string(e.code);
  if (e.message.starts_with(code)) return QString::fromStdString(e.message);
  return QString::fromStdString(code + ": " + e.message);
}

}  // namespace

DocumentSession::DocumentSession(engine::PdfiumWorker& worker, UiDispatcher& dispatcher,
                                 QObject* parent)
    : QObject(parent), worker_(worker), dispatcher_(dispatcher) {}

DocumentSession::~DocumentSession() {
  pending_open_.cancel();
  pending_render_.cancel();
  handle_.close();
}

void DocumentSession::open(const QString& path, const QString& password) {
  pending_open_.cancel();
  const std::uint64_t gen = ++open_generation_;
  UiDispatcher* dispatcher = &dispatcher_;
  QPointer<DocumentSession> self(this);

  pending_open_ = engine::DocumentHandle::open(
      worker_, onpdf::path_from_utf8(path.toUtf8().toStdString()), password.toStdString(),
      engine::Priority::kHigh,
      // 워커 스레드: 결과를 메인 스레드로 넘기기만 한다.
      [dispatcher, self, gen, path](onpdf::Result<engine::DocumentHandle> result) {
        auto shared = std::make_shared<onpdf::Result<engine::DocumentHandle>>(std::move(result));
        dispatcher->post_to(self, [gen, path, shared](DocumentSession* s) {
          if (gen != s->open_generation_) return;  // 늦게 도착한 이전 요청
          onpdf::Result<engine::DocumentHandle>& r = *shared;
          if (r) {
            // 이전 문서의 렌더가 실행 중이었다면 그 결과는 세대 불일치로 버려진다
            s->pending_render_.cancel();
            ++s->render_generation_;
            s->handle_.close();
            s->handle_ = std::move(*r);
            s->path_ = path;
            emit s->opened(path, s->handle_.info().page_count);
            return;
          }
          switch (r.error().code) {
            case onpdf::ErrorCode::kPasswordRequired:
              emit s->password_required(path);
              break;
            case onpdf::ErrorCode::kCancelled:
              break;
            default:
              emit s->open_failed(path, error_text(r.error()));
              break;
          }
        });
      });
}

void DocumentSession::render_page(int page_index, float scale, qreal device_pixel_ratio) {
  if (!handle_.valid() || !(scale > 0.0f)) return;
  pending_render_.cancel();
  const std::uint64_t gen = ++render_generation_;
  const auto started = std::chrono::steady_clock::now();
  UiDispatcher* dispatcher = &dispatcher_;
  QPointer<DocumentSession> self(this);

  engine::RenderOptions options;
  options.scale = scale;
  pending_render_ = handle_.render_page(
      page_index, options, engine::Priority::kHigh,
      // 워커 스레드
      [dispatcher, self, gen, page_index, device_pixel_ratio,
       started](onpdf::Result<onpdf::Bitmap> result) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        auto shared = std::make_shared<onpdf::Result<onpdf::Bitmap>>(std::move(result));
        dispatcher->post_to(
            self, [gen, page_index, device_pixel_ratio, elapsed, shared](DocumentSession* s) {
              if (gen != s->render_generation_) return;  // 취소되었거나 더 새 요청이 있음
              onpdf::Result<onpdf::Bitmap>& r = *shared;
              if (!r) {
                if (r.error().code != onpdf::ErrorCode::kCancelled) {
                  emit s->render_failed(page_index, error_text(r.error()));
                }
                return;
              }
              emit s->page_rendered(page_index, to_qimage(std::move(*r), device_pixel_ratio),
                                    static_cast<int>(elapsed.count()));
            });
      });
}

void DocumentSession::close() {
  pending_open_.cancel();
  pending_render_.cancel();
  ++open_generation_;
  ++render_generation_;
  handle_.close();
  handle_ = engine::DocumentHandle{};
  path_.clear();
}

}  // namespace onpdf::app
