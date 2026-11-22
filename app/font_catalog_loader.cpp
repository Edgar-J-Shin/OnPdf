#include "font_catalog_loader.h"

#include "ui_dispatcher.h"
#include <QPointer>

namespace onpdf::app {

FontCatalogLoader::FontCatalogLoader(UiDispatcher& dispatcher, QObject* parent)
    : QObject(parent), dispatcher_(dispatcher) {}

FontCatalogLoader::~FontCatalogLoader() {
  ++generation_;  // 늦게 오는 결과는 버린다 (jthread 소멸이 join 한다)
}

void FontCatalogLoader::start(std::vector<platform::FontDir> dirs) {
  if (active_) return;
  if (thread_.joinable()) thread_.join();
  active_ = true;
  const int generation = ++generation_;
  UiDispatcher* dispatcher = &dispatcher_;
  QPointer<FontCatalogLoader> self(this);
  thread_ = std::jthread([dirs = std::move(dirs), dispatcher, self, generation] {
    auto catalog = std::make_shared<const edit::FontCatalog>(edit::FontCatalog::scan(dirs));
    dispatcher->post_to(self, [catalog, generation](FontCatalogLoader* loader) {
      if (generation != loader->generation_) return;
      loader->catalog_ = catalog;
      loader->active_ = false;
      emit loader->ready();
    });
  });
}

}  // namespace onpdf::app
