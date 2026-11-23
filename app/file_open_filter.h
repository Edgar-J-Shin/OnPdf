#pragma once

// macOS 는 Finder 의 "다음으로 열기"·Dock 아이콘에 끌어 놓기·이미 실행 중일 때 연 문서를 명령줄
// 인자가 아니라 QFileOpenEvent 로 보낸다 (Info.plist 의 CFBundleDocumentTypes, #90). 다른
// 플랫폼에서는 오지 않는다. QApplication 에 설치한다: app.installEventFilter(&filter). 창보다 늦게
// 만들어 먼저 소멸하게 한다

#include "main_window.h"
#include <QEvent>
#include <QFileOpenEvent>
#include <QObject>

namespace onpdf::app {

class FileOpenFilter final : public QObject {
 public:
  explicit FileOpenFilter(MainWindow& window) : window_(window) {}

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (event->type() == QEvent::FileOpen) {
      const QString file = static_cast<QFileOpenEvent*>(event)->file();
      if (!file.isEmpty()) window_.open_file(file);
      return true;
    }
    return QObject::eventFilter(watched, event);
  }

 private:
  MainWindow& window_;
};

}  // namespace onpdf::app
