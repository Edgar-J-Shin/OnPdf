#include "onpdf/core/view/tile_source.h"

#include "onpdf/core/engine/pdfium_document.h"

namespace onpdf::view {

engine::TaskHandle SyncTileSource::request(const TileRequest& request, engine::Priority,
                                           TileCallback on_done) {
  engine::RenderOptions options;
  options.scale = static_cast<float>(dequantize_scale(request.key.scale_q));
  on_done(request.key, document_.render_region(request.key.page, options,
                                               tile_rect_px(request.key, request.page_pt)));
  return engine::TaskHandle{};
}

}  // namespace onpdf::view
