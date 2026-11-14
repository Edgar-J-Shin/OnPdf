// app 테스트 진입점. QImage·QObject 이벤트 루프가 필요하므로 QGuiApplication 을 만든다.
// 화면이 없는 CI 를 위해 offscreen 플랫폼을 기본으로 한다.

#include <gtest/gtest.h>

#include <QGuiApplication>
#include <QtGlobal>

int main(int argc, char** argv) {
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
  QGuiApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
