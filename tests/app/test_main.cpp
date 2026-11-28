// app 테스트 진입점. 위젯(DocumentView) 테스트가 있으므로 QApplication 을 만든다.
// 화면이 없는 CI 를 위해 offscreen 플랫폼을 기본으로 한다.

#include "onpdf/core/engine/render_child.h"

#include "dialog_helpers.h"
#include "i18n.h"
#include "main_window.h"
#include "settings.h"
#include <QApplication>
#include <QDialog>
#include <QMetaObject>
#include <QTemporaryDir>
#include <QtGlobal>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>
#include <mutex>
#include <string>
#include <thread>

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

// 멈춤 감시 (#145). ctest 시간 초과는 어느 테스트가 어디서 멈췄는지 아무것도 남기지 않는다.
//  - 한 테스트가 60초를 넘기면(spin_until 상한 30초의 두 배) 메인 스레드에 작업을 보낸다 —
//  대화상자의
//    exec() 같은 중첩 이벤트 루프 안이라도 돈다: 열린 창을 stderr 에 적고 보이는 대화상자를 전부
//    거절해 테스트가 실패로 끝나게 한다
//  - 120초에도 안 끝나면(메인 스레드가 join 등에 막혀 이벤트를 돌리지 못함) 테스트 이름을 남기고
//    프로세스를 끝낸다 (abort 는 Windows 에서 WER 창이 뜰 수 있어 _Exit)
class HangWatchdog : public ::testing::EmptyTestEventListener {
 public:
  void OnTestStart(const ::testing::TestInfo& info) override {
    name_ = std::string(info.test_suite_name()) + "." + info.name();
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
  }
  void OnTestEnd(const ::testing::TestInfo&) override {
    thread_.request_stop();
    cv_.notify_all();
    thread_.join();
  }

 private:
  // 진단까지의 초 (기본 60, 그 두 배에 종료). 환경 변수 ONPDF_TEST_HANG_SECONDS 로 줄여 감시 자체를
  // 확인할 수 있다
  static std::chrono::seconds diagnose_after() {
    const int env = qEnvironmentVariableIntValue("ONPDF_TEST_HANG_SECONDS");
    return std::chrono::seconds(env > 0 ? env : 60);
  }

  void run(std::stop_token stop) {
    const auto wait = diagnose_after();
    std::unique_lock lock(mutex_);
    if (cv_.wait_for(lock, stop, wait, [&] { return stop.stop_requested(); })) return;
    const std::string name = name_;
    const long long seconds = wait.count();
    QMetaObject::invokeMethod(
        qApp,
        [name, seconds] {
          std::fprintf(stderr, "[  HANG?   ] %s has run for %lld s; open windows: %s\n",
                       name.c_str(), seconds,
                       onpdf::test::describe_open_windows().toUtf8().constData());
          for (QWidget* w : QApplication::topLevelWidgets()) {
            if (!w->isVisible()) continue;
            if (auto* dialog = qobject_cast<QDialog*>(w)) {
              dialog->reject();
            } else if (w->isModal()) {
              w->close();
            }
          }
          std::fflush(stderr);
        },
        Qt::QueuedConnection);
    if (cv_.wait_for(lock, stop, wait, [&] { return stop.stop_requested(); })) return;
    std::fprintf(stderr, "[  HANG    ] %s did not finish in %lld s; exiting\n", name.c_str(),
                 static_cast<long long>(2 * wait.count()));
    std::fflush(stderr);
    std::_Exit(3);
  }

  std::string name_;
  std::mutex mutex_;
  std::condition_variable_any cv_;
  std::jthread thread_;
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
  ::testing::UnitTest::GetInstance()->listeners().Append(new HangWatchdog());  // #145
  return RUN_ALL_TESTS();
}
