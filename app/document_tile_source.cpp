#include "document_tile_source.h"

#include <memory>
#include <utility>

namespace onpdf::app {

DocumentTileSource::DocumentTileSource(engine::DocumentHandle handle, UiDispatcher& dispatcher,
                                       QObject* owner)
    : handle_(std::move(handle)), dispatcher_(dispatcher), owner_(owner) {}

engine::TaskHandle DocumentTileSource::request(const view::TileRequest& request,
                                               engine::Priority priority, view::TileCallback on_done) {
  engine::RenderOptions options;
  options.scale = static_cast<float>(view::dequantize_scale(request.key.scale_q));
  const view::TileKey key = request.key;
  UiDispatcher* dispatcher = &dispatcher_;
  QPointer<QObject> owner = owner_;
  return handle_.render_region(
      key.page, options, view::tile_rect_px(key, request.page_pt), priority,
      // 워커 스레드: 결과를 메인 스레드로 넘기기만 한다
      [dispatcher, owner, key, on_done = std::move(on_done)](onpdf::Result<onpdf::Bitmap> result) {
        auto shared = std::make_shared<onpdf::Result<onpdf::Bitmap>>(std::move(result));
        dispatcher->post_to(owner, [key, on_done, shared](QObject*) { on_done(key, std::move(*shared)); });
      });
}

}  // namespace onpdf::app
