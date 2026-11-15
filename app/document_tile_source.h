#pragma once

// core::TileSource 구현: DocumentHandle::render_region 으로 워커에 요청하고,
// 워커 콜백을 UiDispatcher 로 메인 스레드에 돌려준 뒤 스케줄러 콜백을 부른다.

#include "onpdf/core/engine/document_handle.h"
#include "onpdf/core/view/tile_source.h"
#include "ui_dispatcher.h"

#include <QObject>
#include <QPointer>

namespace onpdf::app {

class DocumentTileSource : public view::TileSource {
 public:
  // owner: 콜백 수신 보호용 QObject (DocumentView). 소멸하면 늦은 타일은 버려진다.
  DocumentTileSource(engine::DocumentHandle handle, UiDispatcher& dispatcher, QObject* owner);

  engine::TaskHandle request(const view::TileRequest& request, engine::Priority priority,
                             view::TileCallback on_done) override;

 private:
  engine::DocumentHandle handle_;
  UiDispatcher& dispatcher_;
  QPointer<QObject> owner_;
};

}  // namespace onpdf::app
