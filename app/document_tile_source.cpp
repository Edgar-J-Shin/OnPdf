#include "document_tile_source.h"

#include <memory>
#include <utility>

namespace onpdf::app {

DocumentTileSource::DocumentTileSource(engine::DocumentHandle handle, UiDispatcher& dispatcher,
                                       QObject* owner)
    : handle_(std::move(handle)), dispatcher_(dispatcher), owner_(owner) {}

engine::TaskHandle DocumentTileSource::request(const view::TileRequest& request,
                                               engine::Priority priority,
                                               view::TileCallback on_done) {
  engine::RenderOptions options;
  options.scale = static_cast<float>(view::dequantize_scale(request.key.scale_q));
  const view::TileKey key = request.key;
  UiDispatcher* dispatcher = &dispatcher_;
  QPointer<QObject> owner = owner_;
  RefusedCallback on_refused = on_refused_;
  std::weak_ptr<bool> alive = alive_;
  return handle_.render_region(
      key.page, options, view::tile_rect_px(key, request.page_pt), priority,
      // 워커 스레드: 결과를 메인 스레드로 넘기기만 한다
      [dispatcher, owner, key, on_done = std::move(on_done), on_refused = std::move(on_refused),
       alive](onpdf::Result<onpdf::Bitmap> result) {
        auto shared = std::make_shared<onpdf::Result<onpdf::Bitmap>>(std::move(result));
        dispatcher->post_to(owner, [key, on_done, on_refused, alive, shared](QObject*) {
          // 거부 통지는 이 소스가 아직 살아 있을 때만 (문서를 바꾼 뒤 온 옛 문서의 결과는 무시).
          // 스케줄러 콜백은 자체 weak_ptr 로 같은 보호를 한다
          if (!*shared && (*shared).error().code == onpdf::ErrorCode::kRenderRefused &&
              on_refused && !alive.expired()) {
            on_refused(key.page, QString::fromStdString((*shared).error().message));
          }
          on_done(key, std::move(*shared));
        });
      });
}

}  // namespace onpdf::app
