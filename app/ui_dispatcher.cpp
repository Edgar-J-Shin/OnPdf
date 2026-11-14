#include "ui_dispatcher.h"

#include <QMetaObject>

namespace onpdf::app {

UiDispatcher::UiDispatcher(QObject* parent) : QObject(parent) {}

void UiDispatcher::post(std::function<void()> fn) {
  QMetaObject::invokeMethod(this, std::move(fn), Qt::QueuedConnection);
}

}  // namespace onpdf::app
