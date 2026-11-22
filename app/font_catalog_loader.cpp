#include "font_catalog_loader.h"

#include "ui_dispatcher.h"
#include <QPointer>

namespace onpdf::app {

FontCatalogLoader::FontCatalogLoader(UiDispatcher& dispatcher, QObject* parent)
    : QObject(parent), dispatcher_(dispatcher) {}

FontCatalogLoader::~FontCatalogLoader() {
  ++generation_;  // 늦게 오는 결과는 버린다
  if (thread_.joinable())
    thread_.request_stop();  // 스캔 중이면 남은 파일을 건너뛰고 끝난다 (jthread 소멸이 join 한다)
}

void FontCatalogLoader::start(std::vector<platform::FontDir> dirs) {
  if (active_) return;
  if (thread_.joinable()) thread_.join();
  active_ = true;
  const int generation = ++generation_;
  UiDispatcher* dispatcher = &dispatcher_;
  QPointer<FontCatalogLoader> self(this);
  thread_ =
      std::jthread([dirs = std::move(dirs), dispatcher, self, generation](std::stop_token stop) {
        auto catalog = std::make_shared<const edit::FontCatalog>(edit::FontCatalog::scan(
            dirs, edit::FontCatalog::kMinGlyphs, [&stop] { return stop.stop_requested(); }));
        if (stop.stop_requested()) return;  // 소멸 중: 결과를 넘기지 않는다
        dispatcher->post_to(self, [catalog, generation](FontCatalogLoader* loader) {
          if (generation != loader->generation_) return;
          loader->catalog_ = catalog;
          loader->active_ = false;
          emit loader->ready();
        });
      });
}

}  // namespace onpdf::app
