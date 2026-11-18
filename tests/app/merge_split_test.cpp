// 병합·분할 (#37): 결과 열기, 암호 오류 안내, 입력=출력 거절, 분할 파일 생성·완료 창, 수정 문서 분할 전 저장 확인
#include "main_window.h"
#include "document_view.h"
#include "outline_panel.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/platform/path.h"
#include "ui_dispatcher.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QEventLoop>
#include <QMessageBox>
#include <QStatusBar>
#include <QTimer>

#include <filesystem>

namespace onpdf::app {
namespace {

using onpdf::test::ScopedTempDir;
using onpdf::test::testdata;

template <class Pred>
bool spin_until(Pred pred, int timeout_ms = 15000) {
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

// 이름이 object_name 인 메시지 상자가 뜨면 role 버튼을 누르고 본문을 기록한다
void answer_box(const char* object_name, QMessageBox::ButtonRole role, QString* text, bool* answered) {
  auto* poll = new QTimer();
  poll->setInterval(10);
  QObject::connect(poll, &QTimer::timeout, poll, [poll, object_name, role, text, answered] {
    auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
    if (box == nullptr || box->objectName() != QString::fromLatin1(object_name)) return;
    if (text != nullptr) *text = box->text();
    for (QAbstractButton* b : box->buttons()) {
      if (box->buttonRole(b) == role) {
        if (answered != nullptr) *answered = true;
        b->click();
        break;
      }
    }
    poll->deleteLater();
  });
  poll->start();
}

QString q(const std::filesystem::path& p) { return QString::fromStdString(onpdf::path_to_utf8(p)); }

class MergeSplitTest : public ::testing::Test {
 protected:
  MergeSplitTest() : worker_(options()), window_(worker_, dispatcher_) {
    window_.resize(900, 900);
    window_.show();
  }
  static engine::PdfiumWorker::Options options() {
    engine::PdfiumWorker::Options o;
    o.use_system_fonts = false;
    return o;
  }
  bool open_path(const std::filesystem::path& path) {
    const QString file = q(path.filename());
    window_.open_file(q(path));
    return spin_until([&] { return window_.windowTitle().contains(file) && window_.document_view()->has_document(); });
  }
  std::filesystem::path open_copy(const char* name) {
    const auto copy = dir_ / name;
    std::filesystem::copy_file(testdata(name), copy);
    EXPECT_TRUE(open_path(copy));
    return copy;
  }
  bool wait_job() { return spin_until([&] { return !window_.structure_job()->active(); }); }

  ScopedTempDir dir_;
  UiDispatcher dispatcher_;
  engine::PdfiumWorker worker_;
  MainWindow window_;
};

TEST_F(MergeSplitTest, MergeOpensResultWithPrimaryOutline) {
  const auto out = dir_ / "merged.pdf";
  ASSERT_TRUE(window_.merge_files({{testdata("bookmarks.pdf"), ""}, {testdata("encrypted-aes256.pdf"), "user"},
                                   {testdata("korean-embedded.pdf"), ""}},
                                  q(out)));
  EXPECT_TRUE(window_.structure_job()->active());
  EXPECT_FALSE(window_.merge_action()->isEnabled()) << "작업 중에는 다시 시작하지 못한다";
  ASSERT_TRUE(wait_job());
  ASSERT_TRUE(spin_until([&] { return window_.windowTitle().contains(QStringLiteral("merged.pdf")) && window_.document_view()->has_document(); }));
  EXPECT_EQ(window_.document_view()->page_count(), 13);
  ASSERT_TRUE(spin_until([&] { return window_.outline_panel()->row_count() == 15; })) << "첫 문서의 목차가 유지된다";
  EXPECT_TRUE(window_.merge_action()->isEnabled());
  EXPECT_TRUE(std::filesystem::exists(out));
}

TEST_F(MergeSplitTest, MergeFailureIsExplainedAndLeavesNoFile) {
  const auto out = dir_ / "merged.pdf";
  QString message;
  bool answered = false;
  answer_box("structure_failed", QMessageBox::AcceptRole, &message, &answered);
  ASSERT_TRUE(window_.merge_files({{testdata("simple.pdf"), ""}, {testdata("encrypted-aes256.pdf"), "wrong"}}, q(out)));
  ASSERT_TRUE(spin_until([&] { return answered && !window_.structure_job()->active(); }));
  EXPECT_TRUE(message.contains(QStringLiteral("암호"))) << message.toStdString();
  EXPECT_TRUE(message.contains(QStringLiteral("encrypted-aes256.pdf"))) << message.toStdString();
  EXPECT_FALSE(std::filesystem::exists(out));
  // 입력 = 출력은 시작조차 하지 않는다
  const auto copy = dir_ / "a.pdf";
  std::filesystem::copy_file(testdata("simple.pdf"), copy);
  EXPECT_FALSE(window_.merge_files({{copy, ""}}, q(copy)));
  EXPECT_TRUE(window_.statusBar()->currentMessage().contains(QStringLiteral("같습니다")));
  EXPECT_FALSE(window_.merge_files({}, q(out)));
}

TEST_F(MergeSplitTest, SplitWritesPiecesAndReports) {
  open_copy("bookmarks.pdf");
  const auto out_dir = dir_ / "pieces";
  std::filesystem::create_directory(out_dir);
  QString message;
  bool answered = false;
  answer_box("split_done", QMessageBox::AcceptRole, &message, &answered);
  ASSERT_TRUE(window_.split_document(3, q(out_dir)));
  ASSERT_TRUE(spin_until([&] { return answered && !window_.structure_job()->active(); }));
  EXPECT_TRUE(message.contains(QStringLiteral("4개"))) << message.toStdString();
  for (const char* name : {"bookmarks-1.pdf", "bookmarks-2.pdf", "bookmarks-3.pdf", "bookmarks-4.pdf"}) {
    EXPECT_TRUE(std::filesystem::exists(out_dir / name)) << name;
  }
  std::size_t n = 0;
  for ([[maybe_unused]] const auto& e : std::filesystem::directory_iterator(out_dir)) ++n;
  EXPECT_EQ(n, 4u) << "임시 파일이 남지 않는다";
  EXPECT_EQ(window_.document_view()->page_count(), 10) << "현재 문서는 그대로";
  EXPECT_FALSE(window_.isWindowModified());
  EXPECT_FALSE(window_.split_document(0, q(out_dir)));
}

TEST_F(MergeSplitTest, SplitAsksToSaveModifiedDocumentFirst) {
  open_copy("simple.pdf");
  window_.rotate_current_page(1);
  ASSERT_TRUE(spin_until([&] { return !window_.edit_controller()->busy(); }));
  ASSERT_TRUE(window_.isWindowModified());
  bool asked = false;
  answer_box("save_changes_question", QMessageBox::RejectRole, nullptr, &asked);  // 취소 → 분할하지 않는다
  window_.split_action()->trigger();
  ASSERT_TRUE(spin_until([&] { return asked; }));
  EXPECT_FALSE(window_.structure_job()->active());
  EXPECT_TRUE(window_.isWindowModified());
}

TEST_F(MergeSplitTest, SplitActionNeedsDocument) {
  EXPECT_FALSE(window_.split_action()->isEnabled());
  EXPECT_TRUE(window_.merge_action()->isEnabled());
  ASSERT_TRUE(open_path(testdata("simple.pdf")));
  EXPECT_TRUE(window_.split_action()->isEnabled());
}

}  // namespace
}  // namespace onpdf::app
