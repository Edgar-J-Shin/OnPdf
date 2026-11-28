// app 테스트 진입점. 위젯(DocumentView) 테스트가 있으므로 QApplication 을 만든다.
// 화면이 없는 CI 를 위해 offscreen 플랫폼을 기본으로 한다.

#include "onpdf/core/engine/render_child.h"

#include "i18n.h"
#include "main_window.h"
#include "settings.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QtGlobal>
#include <cstdio>
#include <cstring>
#include <gtest/gtest.h>

namespace {

// 테스트마다 새 INI — 어떤 테스트가 툴바 기본값·창 상태를 저장해도 다음 테스트로 새지 않는다 (#129)
class FreshSettingsPerTest : public ::testing::EmptyTestEventListener {
 public:
  explicit FreshSettingsPerTest(QString dir) : dir_(std::move(dir)) {}
  void OnTestStart(const ::testing::TestInfo&) override {
    onpdf::app::Settings::set_default_path(dir_ +
                                           QStringLiteral("/settings-%1.ini").arg(++serial_));
  }

 private:
  QString dir_;
  int serial_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
  // 렌더 자식 프로세스 (#137): 앱 테스트는 이 바이너리를 자식으로 띄운다
  if (argc >= 2 && std::strcmp(argv[1], "--render-child") == 0)
    return onpdf::engine::run_render_child(argc, argv);
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  // 설정(#129)은 임시 INI 로 격리한다 — 개발자 머신의 설정을 읽지도 바꾸지도 않는다
  QTemporaryDir settings_dir;
  if (!settings_dir.isValid()) {
    std::fprintf(stderr, "cannot create a temporary settings directory\n");
    return 1;
  }
  onpdf::app::Settings::set_default_path(settings_dir.filePath(QStringLiteral("settings.ini")));
  onpdf::app::MainWindow::set_default_render_child_system_fonts(false);  // 워커와 같은 글꼴 (#137)
  // 앱 테스트는 한국어 UI 로 돈다 (기존 테스트가 한국어 문구를 비교한다, #8). 번역 리소스가 빠지면
  // 여기서 실패한다
  if (!onpdf::app::set_language(app, onpdf::app::Language::kKorean)) {
    std::fprintf(stderr, "Korean translation resource is missing\n");
    return 1;
  }
  ::testing::InitGoogleTest(&argc, argv);
  ::testing::UnitTest::GetInstance()->listeners().Append(
      new FreshSettingsPerTest(settings_dir.path()));  // gtest 가 소유·해제한다
  return RUN_ALL_TESTS();
}
