// app 테스트 진입점. 위젯(DocumentView) 테스트가 있으므로 QApplication 을 만든다.
// 화면이 없는 CI 를 위해 offscreen 플랫폼을 기본으로 한다.

#include "i18n.h"
#include <QApplication>
#include <QtGlobal>
#include <cstdio>
#include <gtest/gtest.h>

int main(int argc, char** argv) {
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  // 앱 테스트는 한국어 UI 로 돈다 (기존 테스트가 한국어 문구를 비교한다, #8). 번역 리소스가 빠지면
  // 여기서 실패한다
  if (!onpdf::app::set_language(app, onpdf::app::Language::kKorean)) {
    std::fprintf(stderr, "Korean translation resource is missing\n");
    return 1;
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
