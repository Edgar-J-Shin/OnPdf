#pragma once

// core::TileSource 구현: DocumentHandle::render_region 으로 워커에 요청하고,
// 워커 콜백을 UiDispatcher 로 메인 스레드에 돌려준 뒤 스케줄러 콜백을 부른다.

#include "onpdf/core/engine/document_handle.h"
#include "onpdf/core/view/tile_source.h"

#include "ui_dispatcher.h"
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>
#include <memory>
#include <utility>

namespace onpdf::app {

class DocumentTileSource : public view::TileSource {
 public:
  // owner: 콜백 수신 보호용 QObject (DocumentView). 소멸하면 늦은 타일은 버려진다.
  DocumentTileSource(engine::DocumentHandle handle, UiDispatcher& dispatcher, QObject* owner);

  engine::TaskHandle request(const view::TileRequest& request, engine::Priority priority,
                             view::TileCallback on_done) override;

  // 렌더 거부(kRenderRefused, #121) 를 뷰에 알린다 (메인 스레드, 스케줄러 콜백 직전). 스케줄러는
  // 오류 코드를 버리고 실패 집합에만 넣으므로 여기서 따로 전한다. 이 소스가 소멸한 뒤(문서 교체)
  // 도착한 결과는 알리지 않는다 — 옛 문서의 쪽 번호가 새 문서에 붙으면 안 된다
  using RefusedCallback = std::function<void(int page, const QString& reason)>;
  void set_on_refused(RefusedCallback on_refused) { on_refused_ = std::move(on_refused); }

 private:
  engine::DocumentHandle handle_;
  UiDispatcher& dispatcher_;
  QPointer<QObject> owner_;
  RefusedCallback on_refused_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);  // 늦은 거부 통지 보호 (#121)
};

}  // namespace onpdf::app
