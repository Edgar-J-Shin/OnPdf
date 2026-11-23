#pragma once

// 시스템 글꼴 카탈로그를 별도 스레드에서 한 번 훑어 메인 스레드로 넘긴다 (#73). StructureJob 과
// 같은 패턴: jthread + UiDispatcher::post_to(QPointer) + 세대. PDFium 워커와 무관(HarfBuzz 만)

#include "onpdf/edit/font_catalog.h"

#include <QObject>
#include <memory>
#include <thread>
#include <vector>

namespace onpdf::app {

class UiDispatcher;

class FontCatalogLoader : public QObject {
  Q_OBJECT
 public:
  explicit FontCatalogLoader(UiDispatcher& dispatcher, QObject* parent = nullptr);
  ~FontCatalogLoader() override;  // 스캔 중이면 중단을 요청하고 기다린다 (파일 하나 단위)

  void start(std::vector<platform::FontDir> dirs);  // 이미 돌고 있으면 무시
  [[nodiscard]] std::shared_ptr<const edit::FontCatalog> catalog() const noexcept {
    return catalog_;
  }  // ready 뒤 유효

 signals:
  void ready();

 private:
  UiDispatcher& dispatcher_;
  std::jthread thread_;
  bool active_ = false;
  int generation_ = 0;
  std::shared_ptr<const edit::FontCatalog> catalog_;
};

}  // namespace onpdf::app
