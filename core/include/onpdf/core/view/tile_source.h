#pragma once

// 타일 렌더 요청 인터페이스. 구현은 콜백을 **스케줄러를 소유한 스레드** 에서 호출해야 한다.
//  - SyncTileSource: PdfiumDocument 를 직접 호출해 즉시 콜백 (CLI·단일 스레드 테스트)
//  - app::DocumentTileSource: DocumentHandle 로 워커에 보내고 UiDispatcher 로 메인 스레드에 돌려줌

#include "onpdf/core/bitmap.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/engine/render_types.h"
#include "onpdf/core/error.h"
#include "onpdf/core/view/tile.h"

#include <functional>

namespace onpdf::engine {
class PdfiumDocument;
}

namespace onpdf::view {

struct TileRequest {
  TileKey key;
  engine::PageSize page_pt;
};

using TileCallback = std::function<void(const TileKey&, Result<Bitmap>)>;

class TileSource {
 public:
  virtual ~TileSource() = default;
  virtual engine::TaskHandle request(const TileRequest& request, engine::Priority priority,
                                     TileCallback on_done) = 0;
};

class SyncTileSource : public TileSource {
 public:
  explicit SyncTileSource(const engine::PdfiumDocument& document) : document_(document) {}
  engine::TaskHandle request(const TileRequest& request, engine::Priority priority,
                             TileCallback on_done) override;

 private:
  const engine::PdfiumDocument& document_;
};

}  // namespace onpdf::view
