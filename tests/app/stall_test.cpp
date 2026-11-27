// 악성 PDF 완화 (#121): 워커 멈춤 감시(렌더 작업이 상한을 넘으면 "응답 없음" 상자, 강제 종료는 훅,
// 닫기·열기는 상자로 수렴, 저장 종류는 경고 없음)와 렌더 거부 쪽 표시(pattern-bomb.pdf).

#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/platform/path.h"

#include "../test_support.h"
#include "dialog_helpers.h"
#include "document_view.h"
#include "main_window.h"
#include "ui_dispatcher.h"
#include <QAbstractButton>
#include <QApplication>
#include <QEventLoop>
#include <QMessageBox>
#include <QStatusBar>
#include <QStringList>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <gtest/gtest.h>
#include <latch>
#include <memory>
#include <vector>

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

// 워커를 막는 작업 (종류 지정). 소멸자가 풀어 워커가 hang 하지 않는다
class Block {
 public:
  Block(engine::PdfiumWorker& worker, engine::TaskKind kind)
      : entered_(std::make_shared<std::latch>(1)), release_(std::make_shared<std::latch>(1)) {
    worker.post(
        [entered = entered_, release = release_](bool) {
          entered->count_down();
          release->wait();
        },
        engine::Priority::kNormal, kind);
    entered_->wait();
  }
  ~Block() { open(); }
  void open() {
    if (!opened_) {
      opened_ = true;
      release_->count_down();
    }
  }

 private:
  std::shared_ptr<std::latch> entered_;
  std::shared_ptr<std::latch> release_;
  bool opened_ = false;
};

class StallTest : public ::testing::Test {
 protected:
  StallTest() : worker_(options()), window_(worker_, dispatcher_) {
    window_.resize(900, 900);
    window_.show();
    window_.set_stall_threshold(200ms);
    window_.set_force_exit_hook([this](int code) { exit_codes_.push_back(code); });
  }
  static engine::PdfiumWorker::Options options() {
    engine::PdfiumWorker::Options o;
    o.use_system_fonts = false;
    return o;
  }
  bool open(const char* name) {
    const auto path = testdata(name);
    window_.open_file(QString::fromStdString(onpdf::path_to_utf8(path)));
    return spin_until([&] { return window_.document_view()->has_document(); });
  }
  static QAbstractButton* button_with_role(QMessageBox* box, QMessageBox::ButtonRole role) {
    for (QAbstractButton* b : box->buttons()) {
      if (box->buttonRole(b) == role) return b;
    }
    return nullptr;
  }

  UiDispatcher dispatcher_;
  engine::PdfiumWorker worker_;
  MainWindow window_;
  std::vector<int> exit_codes_;
};

TEST_F(StallTest, WarnsOnceWhenRenderTaskStallsAndRecovers) {
  ASSERT_TRUE(open("simple.pdf"));
  Block block(worker_, engine::TaskKind::kRender);
  ASSERT_TRUE(spin_until([] { return find_dialog("worker_stalled") != nullptr; }, 5000));
  EXPECT_TRUE(window_.worker_stalled());
  auto* box = qobject_cast<QMessageBox*>(find_dialog("worker_stalled"));
  ASSERT_NE(box, nullptr);
  EXPECT_FALSE(box->isModal());
  // "기다리기": 같은 작업으로는 다시 묻지 않는다
  QAbstractButton* wait = button_with_role(box, QMessageBox::RejectRole);
  ASSERT_NE(wait, nullptr);
  wait->click();
  ASSERT_TRUE(spin_until([] { return find_dialog("worker_stalled") == nullptr; }, 5000));
  QTimer wait_timer;
  QEventLoop loop;
  QTimer::singleShot(700, &loop, &QEventLoop::quit);  // 임계 200ms 의 세 배 동안
  loop.exec();
  EXPECT_EQ(find_dialog("worker_stalled"), nullptr);
  EXPECT_TRUE(exit_codes_.empty());
  // 작업이 끝나면 멈춤 상태가 풀린다
  block.open();
  ASSERT_TRUE(spin_until([&] { return !window_.worker_stalled(); }, 5000));
}

TEST_F(StallTest, CloseWhileStalledShowsWarningAndForceQuitUsesHook) {
  ASSERT_TRUE(open("simple.pdf"));
  Block block(worker_, engine::TaskKind::kRender);
  ASSERT_TRUE(spin_until([&] { return window_.worker_stalled(); }, 5000));
  // 타이머가 먼저 띄웠을 수 있다 — 어느 쪽이든 닫기는 상자로 간다
  window_.close();
  ASSERT_TRUE(spin_until([] { return find_dialog("worker_stalled") != nullptr; }, 5000));
  EXPECT_TRUE(window_.isVisible());
  auto* box = qobject_cast<QMessageBox*>(find_dialog("worker_stalled"));
  ASSERT_NE(box, nullptr);
  QAbstractButton* quit = button_with_role(box, QMessageBox::DestructiveRole);
  ASSERT_NE(quit, nullptr);
  quit->click();
  ASSERT_TRUE(spin_until([&] { return !exit_codes_.empty(); }, 5000));
  EXPECT_EQ(exit_codes_.front(), 3);
  block.open();
}

TEST_F(StallTest, LongSaveTaskDoesNotWarn) {
  ASSERT_TRUE(open("simple.pdf"));
  Block block(worker_, engine::TaskKind::kSave);
  QEventLoop loop;
  QTimer::singleShot(800, &loop, &QEventLoop::quit);
  loop.exec();
  EXPECT_FALSE(window_.worker_stalled());
  EXPECT_EQ(find_dialog("worker_stalled"), nullptr);
  block.open();
}

TEST_F(StallTest, RefusedPageShowsPlaceholderAndStatus) {
  // 상태 표시줄 메시지는 열기 뒤 정리·선택 상태로 덮일 수 있어 지나간 메시지를 모아 본다
  QStringList messages;
  QObject::connect(window_.statusBar(), &QStatusBar::messageChanged, &window_,
                   [&messages](const QString& m) { messages << m; });
  ASSERT_TRUE(open("pattern-bomb.pdf"));
  auto* view = window_.document_view();
  view->go_to_page(1);
  ASSERT_TRUE(spin_until([&] { return view->refused_pages().contains(1); }));
  EXPECT_NE(view->refused_pages().at(1).indexOf(QStringLiteral("XStep")), -1);
  EXPECT_TRUE(spin_until(
      [&] {
        return std::any_of(messages.begin(), messages.end(), [](const QString& m) {
          return m.contains(QStringLiteral("2쪽")) && m.contains(QStringLiteral("표시할 수 없"));
        });
      },
      5000))
      << messages.join(QStringLiteral(" | ")).toStdString();
  // 정상 쪽은 그대로 그려진다 (거부 집합 밖)
  EXPECT_FALSE(view->refused_pages().contains(0));
  EXPECT_FALSE(window_.worker_stalled());
}

}  // namespace
}  // namespace onpdf::app
