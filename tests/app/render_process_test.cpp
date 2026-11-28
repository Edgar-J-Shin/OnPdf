// 렌더 자식 프로세스 (#137) 앱 통합: 켰을 때 타일이 자식에서 오는지, 편집 뒤 타일이 새
// 내용인지, 크래시 주입 → 그 쪽만 거부·나머지 정상·저장 정상, 멈춤 주입 → 예산 뒤 거부, 설정 끔 →
// 워커 경로, 실행 파일이 잘못되면 폴백 안내 + 설정 자동 끔. 자식은 테스트 바이너리 자신(test_main
// 분기)

#include "onpdf/core/engine/render_process.h"

#include "onpdf/core/platform/path.h"

#include "../test_support.h"
#include "dialog_helpers.h"
#include "document_view.h"
#include "main_window.h"
#include "settings.h"
#include "thumbnail_panel.h"
#include "ui_dispatcher.h"
#include <QAction>
#include <QApplication>
#include <QEventLoop>
#include <QImage>
#include <QMouseEvent>
#include <QStatusBar>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

namespace onpdf::app {
namespace {

using namespace std::chrono_literals;
using onpdf::test::find_dialog;
using onpdf::test::testdata;

template <class Pred>
bool spin_until(Pred pred, int timeout_ms = 30000) {
  QEventLoop loop;
  QTimer poll;
  bool timed_out = false;
  QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
    if (pred()) loop.quit();
  });
  poll.start(5);
  QTimer::singleShot(timeout_ms, &loop, [&] {
    timed_out = true;
    loop.quit();
  });
  if (pred()) return true;
  loop.exec();
  return !timed_out;
}

QString qpath(const std::filesystem::path& p) {
  return QString::fromStdString(onpdf::path_to_utf8(p));
}

class RenderProcessAppTest : public ::testing::Test {
 protected:
  RenderProcessAppTest() : worker_(options()) {
    Settings().reset_all();
    Settings().set_render_process(true);  // 기본은 끔 — 이 테스트들은 켠다
    unsetenv("ONPDF_RENDER_CHILD_FAULT");
  }
  ~RenderProcessAppTest() override {
    window_.reset();
    unsetenv("ONPDF_RENDER_CHILD_FAULT");
    Settings().reset_all();
  }
  static engine::PdfiumWorker::Options options() {
    engine::PdfiumWorker::Options o;
    o.use_system_fonts = false;
    return o;
  }
  MainWindow& make_window() {
    window_ = std::make_unique<MainWindow>(worker_, dispatcher_);
    window_->resize(900, 900);
    window_->show();
    return *window_;
  }
  DocumentView* view() { return window_->document_view(); }
  bool open_path(const std::filesystem::path& path) {
    window_->open_file(qpath(path));
    return spin_until([&] {
      return window_->windowTitle().contains(qpath(path.filename())) && view()->has_document();
    });
  }
  bool open(const char* name) { return open_path(testdata(name)); }
  bool wait_visible_tiles() {
    return spin_until(
        [&] { return view()->pending_visible_tiles() == 0 && view()->cache_stats().count > 0; });
  }
  bool idle() { return !window_->edit_controller()->busy() && !window_->saving(); }
  void send_mouse(QEvent::Type type, QPointF pos, Qt::MouseButton button,
                  Qt::MouseButtons buttons) {
    QMouseEvent ev(type, pos, view()->viewport()->mapToGlobal(pos.toPoint()), button, buttons,
                   Qt::NoModifier);
    QApplication::sendEvent(view()->viewport(), &ev);
  }
  void drag(QPointF from, QPointF to) {
    send_mouse(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    send_mouse(QEvent::MouseMove, (from + to) / 2, Qt::NoButton, Qt::LeftButton);
    send_mouse(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
    send_mouse(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
  }
  QStringList messages_;
  void capture_status() {
    QObject::connect(window_->statusBar(), &QStatusBar::messageChanged, window_.get(),
                     [this](const QString& m) { messages_ << m; });
  }
  bool saw(const QString& fragment) {
    return std::any_of(messages_.begin(), messages_.end(),
                       [&](const QString& m) { return m.contains(fragment); });
  }

  onpdf::test::ScopedTempDir dir_;
  UiDispatcher dispatcher_;
  engine::PdfiumWorker worker_;
  std::unique_ptr<MainWindow> window_;
};

TEST_F(RenderProcessAppTest, TilesComeFromChildWhenEnabled) {
  MainWindow& w = make_window();
  ASSERT_TRUE(open("korean-embedded.pdf"));
  ASSERT_TRUE(wait_visible_tiles());
  ASSERT_NE(w.render_process(), nullptr);
  EXPECT_GT(w.render_process()->stats().rendered, 0u);
  EXPECT_EQ(w.render_process()->stats().restarts, 0);
  EXPECT_TRUE(w.render_process()->options().exe.filename().string().find("onpdf_app_tests") !=
              std::string::npos);
  // 썸네일도 자식에서
  ThumbnailPanel* thumbs = w.thumbnail_panel();
  w.sidebar()->setCurrentWidget(thumbs);
  thumbs->request_visible_now();
  EXPECT_TRUE(spin_until([&] { return !thumbs->thumbnail(0).isNull(); }));
}

TEST_F(RenderProcessAppTest, EditedPageRendersNewContent) {
  MainWindow& w = make_window();
  const auto copy = dir_ / "simple.pdf";
  std::filesystem::copy_file(testdata("simple.pdf"), copy);
  ASSERT_TRUE(open_path(copy));
  ASSERT_TRUE(wait_visible_tiles());
  const QImage before = view()->grab().toImage();
  // 큰 사각형을 그린다 → Command → 재동기화 → 타일이 새 내용
  w.tool_action(AnnotationTool::kSquare)->trigger();
  drag(view()->page_point_to_viewport(0, {80, 120}), view()->page_point_to_viewport(0, {500, 600}));
  ASSERT_TRUE(spin_until(
      [&] { return idle() && view()->annotations(0) && view()->annotations(0)->size() == 1; }));
  ASSERT_TRUE(wait_visible_tiles());
  EXPECT_TRUE(spin_until([&] { return view()->grab().toImage() != before; }, 10000));
  EXPECT_EQ(w.render_process()->stats().restarts, 0);
  // 저장도 정상 (워커 경로)
  const auto saved = dir_ / "saved.pdf";
  ASSERT_TRUE(w.save_to(qpath(saved)));
  ASSERT_TRUE(spin_until([&] { return idle() && std::filesystem::exists(saved); }));
}

TEST_F(RenderProcessAppTest, CrashOnOnePageRefusesOnlyThatPage) {
  setenv("ONPDF_RENDER_CHILD_FAULT", "crash:1", 1);  // 2쪽에서 죽는다
  MainWindow& w = make_window();
  capture_status();
  ASSERT_TRUE(open("simple.pdf"));
  ASSERT_TRUE(wait_visible_tiles())  // 1쪽은 정상
      << "pending=" << view()->pending_visible_tiles() << " cached=" << view()->cache_stats().count
      << " restarts=" << w.render_process()->stats().restarts
      << " rendered=" << w.render_process()->stats().rendered
      << " refused=" << view()->refused_pages().size()
      << " unavailable=" << w.render_process()->unavailable();
  view()->go_to_page(1);
  EXPECT_TRUE(spin_until([&] { return view()->refused_pages().contains(1); }, 20000));
  EXPECT_GE(w.render_process()->stats().restarts, 2);
  EXPECT_TRUE(saw(QStringLiteral("다시 시작")));
  // 1쪽은 여전히 그려진다, 문서는 쓸 수 있다
  view()->go_to_page(0);
  EXPECT_TRUE(spin_until([&] { return view()->pending_visible_tiles() == 0; }));
  EXPECT_FALSE(view()->refused_pages().contains(0));
  EXPECT_TRUE(w.document_view()->has_document());
}

TEST_F(RenderProcessAppTest, HangIsCutByTimeout) {
  setenv("ONPDF_RENDER_CHILD_FAULT", "hang:0", 1);
  Settings().set_render_timeout_seconds(5);  // 하한
  make_window();
  const auto t0 = std::chrono::steady_clock::now();
  ASSERT_TRUE(open("simple.pdf"));
  EXPECT_TRUE(spin_until([&] { return view()->refused_pages().contains(0); }, 25000));
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 20s);
  view()->go_to_page(1);  // 2쪽은 정상
  EXPECT_TRUE(spin_until(
      [&] { return view()->pending_visible_tiles() == 0 && view()->cache_stats().count > 0; }));
}

TEST_F(RenderProcessAppTest, DefaultIsOffAndUsesWorker) {
  Settings().reset_all();  // 기본값: 끔
  MainWindow& w = make_window();
  ASSERT_TRUE(open("simple.pdf"));
  ASSERT_TRUE(wait_visible_tiles());
  EXPECT_EQ(w.render_process(), nullptr);
  EXPECT_FALSE(w.document_view()->refused_pages().contains(0));
}

TEST_F(RenderProcessAppTest, BadExecutableFallsBackAndTurnsSettingOff) {
  MainWindow& w = make_window();
  capture_status();
  w.set_render_child_exe(dir_ / "missing-onpdf");
  ASSERT_TRUE(open("simple.pdf"));
  ASSERT_TRUE(wait_visible_tiles());  // 워커 폴백으로 그려진다
  EXPECT_TRUE(spin_until([&] { return !Settings().render_process(); }, 10000));
  EXPECT_TRUE(saw(QStringLiteral("시작할 수 없")));
}

}  // namespace
}  // namespace onpdf::app
