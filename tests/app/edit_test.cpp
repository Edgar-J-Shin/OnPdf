// 편집 기반(#31): 쪽 회전 → 화면 갱신, 실행 취소·다시 실행, 수정 표시, 저장·다른 이름으로 저장, 닫기·열기 전 확인.
// 원본 저장 경로를 쓰는 테스트는 testdata 를 덮어쓰지 않도록 임시 폴더 사본을 연다.

#include "document_view.h"
#include "main_window.h"
#include "onpdf/core/engine/document_handle.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/platform/path.h"
#include "thumbnail_panel.h"
#include "ui_dispatcher.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QStatusBar>
#include <QAction>
#include <QApplication>
#include <QEventLoop>
#include <QMessageBox>
#include <QPrinter>
#include <QTimer>

#include <filesystem>
#include <fstream>
#include <string>

namespace onpdf::app {
namespace {

using onpdf::test::read_bytes;
using onpdf::test::ScopedTempDir;
using onpdf::test::testdata;

template <class Pred>
bool spin_until(Pred pred, int timeout_ms = 10000) {
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

// 이름이 object_name 인 메시지 상자가 뜨면 role 버튼을 누른다. 눌렀으면 *answered = true
void answer_dialog(const char* object_name, QMessageBox::ButtonRole role, bool* answered) {
  auto* poll = new QTimer();
  poll->setInterval(10);
  QObject::connect(poll, &QTimer::timeout, poll, [poll, object_name, role, answered] {
    auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
    if (box == nullptr || box->objectName() != QString::fromLatin1(object_name)) return;
    for (QAbstractButton* button : box->buttons()) {
      if (box->buttonRole(button) == role) {
        *answered = true;
        button->click();
        break;
      }
    }
    poll->deleteLater();
  });
  poll->start();
}

class EditTest : public ::testing::Test {
 protected:
  EditTest() : worker_(options()), window_(worker_, dispatcher_) {
    window_.resize(900, 900);
    window_.show();
  }
  static engine::PdfiumWorker::Options options() {
    engine::PdfiumWorker::Options o;
    o.use_system_fonts = false;
    return o;
  }
  bool open_path(const std::filesystem::path& path) {
    const QString file = QString::fromStdString(onpdf::path_to_utf8(path.filename()));
    window_.open_file(QString::fromStdString(onpdf::path_to_utf8(path)));
    return spin_until([&] { return window_.windowTitle().contains(file) && window_.document_view()->has_document(); });
  }
  bool open(const char* name) { return open_path(testdata(name)); }
  // 임시 폴더 사본을 연다 (원래 경로로 저장해도 testdata 가 바뀌지 않게)
  std::filesystem::path open_copy(const char* name) {
    const auto copy = dir_ / name;
    std::filesystem::copy_file(testdata(name), copy);
    EXPECT_TRUE(open_path(copy));
    return copy;
  }
  bool idle() { return !window_.edit_controller()->busy() && !window_.saving(); }
  bool rotate(int quarter_turns) {
    window_.rotate_current_page(quarter_turns);
    return spin_until([&] { return idle(); });
  }
  // 저장된 파일의 첫 쪽 폭 (워커로 다시 열어 확인)
  double saved_page0_width(const std::filesystem::path& path) {
    auto future = engine::DocumentHandle::open(worker_, path);
    auto r = future.get();
    return r ? r->info().page_sizes.at(0).width : -1.0;
  }

  ScopedTempDir dir_;
  UiDispatcher dispatcher_;
  engine::PdfiumWorker worker_;
  MainWindow window_;
};

TEST_F(EditTest, RotateUndoRedoUpdatesViewAndModifiedState) {
  ASSERT_TRUE(open("simple.pdf"));
  auto* view = window_.document_view();
  auto* thumbs = window_.findChild<ThumbnailPanel*>();
  ASSERT_NE(thumbs, nullptr);
  EXPECT_FALSE(window_.isWindowModified());
  EXPECT_FALSE(window_.undo_action()->isEnabled());
  const QSize thumb_before = thumbs->thumb_size(0);
  ASSERT_GT(thumb_before.height(), thumb_before.width());  // 세로 쪽
  // 회전 전에 썸네일이 세로 그림으로 그려져 있어야 다시 렌더 여부를 판정할 수 있다
  ASSERT_TRUE(spin_until([&] { return thumbs->cached_count() == 2 && thumbs->pending_requests() == 0; }));
  ASSERT_GT(thumbs->thumbnail(0).height(), thumbs->thumbnail(0).width());
  // 뷰 레이아웃의 1쪽 높이 = 2쪽 위쪽 - 1쪽 위쪽 (간격 포함). 가장 넓은 쪽(2쪽, 792pt)이 그대로라 배율도 그대로
  const auto first_page_height = [&] {
    return view->page_point_to_viewport(1, {0, 0}).y() - view->page_point_to_viewport(0, {0, 0}).y();
  };
  const double height_before = first_page_height();

  ASSERT_TRUE(rotate(1));
  // 뷰 레이아웃도 새 쪽 크기: 1쪽 높이가 792pt → 612pt 비율로 줄었다
  EXPECT_LT(first_page_height(), height_before * 0.85) << "view layout must use the rotated page size";
  EXPECT_TRUE(window_.isWindowModified());
  EXPECT_TRUE(window_.undo_action()->isEnabled());
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("쪽 회전")));
  EXPECT_FALSE(window_.redo_action()->isEnabled());
  const QSize thumb_after = thumbs->thumb_size(0);
  EXPECT_LT(thumb_after.height(), thumb_after.width()) << "thumbnail row must follow the rotated page";
  // 썸네일 그림 자체도 새 방향으로 다시 렌더된다 (이전 세로 그림을 버림)
  ASSERT_TRUE(spin_until([&] {
    const QImage image = thumbs->thumbnail(0);
    return !image.isNull() && image.width() > image.height();
  })) << "thumbnail image was not re-rendered after rotation";

  window_.undo_action()->trigger();
  ASSERT_TRUE(spin_until([&] { return idle(); }));
  EXPECT_FALSE(window_.isWindowModified());  // 저장 지점으로 돌아왔다
  EXPECT_GT(thumbs->thumb_size(0).height(), thumbs->thumb_size(0).width());
  EXPECT_TRUE(window_.redo_action()->isEnabled());

  window_.redo_action()->trigger();
  ASSERT_TRUE(spin_until([&] { return idle(); }));
  EXPECT_TRUE(window_.isWindowModified());
  EXPECT_LT(thumbs->thumb_size(0).height(), thumbs->thumb_size(0).width());
}

TEST_F(EditTest, SaveAsWritesRotationAndClearsModified) {
  ASSERT_TRUE(open("simple.pdf"));
  ASSERT_TRUE(rotate(1));
  const auto target = dir_ / "회전된 사본.pdf";
  ASSERT_TRUE(window_.save_to(QString::fromStdString(onpdf::path_to_utf8(target))));
  ASSERT_TRUE(spin_until([&] { return !window_.saving(); }));
  EXPECT_FALSE(window_.isWindowModified());
  EXPECT_TRUE(window_.windowTitle().contains(QStringLiteral("회전된 사본.pdf")));
  EXPECT_DOUBLE_EQ(saved_page0_width(target), 792.0);
  // 저장한 뒤 실행 취소하면 다시 수정됨
  window_.undo_action()->trigger();
  ASSERT_TRUE(spin_until([&] { return idle(); }));
  EXPECT_TRUE(window_.isWindowModified());
}

TEST_F(EditTest, SaveFailureKeepsModifiedAndReports) {
  ASSERT_TRUE(open("simple.pdf"));
  ASSERT_TRUE(rotate(1));
  bool reported = false;
  answer_dialog("save_failed", QMessageBox::AcceptRole, &reported);
  ASSERT_TRUE(window_.save_to(QString::fromStdString(onpdf::path_to_utf8(dir_ / "no-such-folder" / "x.pdf"))));
  ASSERT_TRUE(spin_until([&] { return reported && !window_.saving(); }));
  EXPECT_TRUE(window_.isWindowModified());
}

TEST_F(EditTest, CloseAsksAndHonoursEachAnswer) {
  const auto copy = open_copy("simple.pdf");
  ASSERT_TRUE(rotate(1));

  // 취소 → 닫히지 않음
  bool asked = false;
  answer_dialog("save_changes_question", QMessageBox::RejectRole, &asked);
  window_.close();
  ASSERT_TRUE(spin_until([&] { return asked; }));
  QApplication::processEvents();
  EXPECT_TRUE(window_.isVisible());

  // 저장 → 원래 경로에 저장한 뒤 닫힘
  asked = false;
  answer_dialog("save_changes_question", QMessageBox::AcceptRole, &asked);
  window_.close();
  ASSERT_TRUE(spin_until([&] { return asked && !window_.isVisible(); }));
  EXPECT_DOUBLE_EQ(saved_page0_width(copy), 792.0);
}

TEST_F(EditTest, DiscardOnCloseLeavesFileUntouched) {
  const auto copy = open_copy("simple.pdf");
  const auto before = read_bytes(copy);
  ASSERT_TRUE(rotate(1));
  bool asked = false;
  answer_dialog("save_changes_question", QMessageBox::DestructiveRole, &asked);  // 저장 안 함
  window_.close();
  ASSERT_TRUE(spin_until([&] { return asked && !window_.isVisible(); }));
  EXPECT_EQ(read_bytes(copy), before);
}

TEST_F(EditTest, OpeningAnotherDocumentAsksToSave) {
  ASSERT_TRUE(open("simple.pdf"));
  ASSERT_TRUE(rotate(1));
  bool asked = false;
  answer_dialog("save_changes_question", QMessageBox::RejectRole, &asked);  // 취소
  window_.open_file(QString::fromStdString(onpdf::path_to_utf8(testdata("bookmarks.pdf"))));
  EXPECT_TRUE(asked);
  EXPECT_TRUE(window_.windowTitle().contains(QStringLiteral("simple.pdf")));
  EXPECT_TRUE(window_.isWindowModified());

  asked = false;
  answer_dialog("save_changes_question", QMessageBox::DestructiveRole, &asked);  // 저장 안 함
  ASSERT_TRUE(open("bookmarks.pdf"));
  EXPECT_TRUE(asked);
  EXPECT_FALSE(window_.isWindowModified());
  EXPECT_FALSE(window_.undo_action()->isEnabled());  // 새 문서: 스택 비움
}

TEST_F(EditTest, ReadOnlyDocumentDisablesEditing) {
  ASSERT_TRUE(open("encrypted-owner-only.pdf"));
  EXPECT_FALSE(window_.rotate_cw_action()->isEnabled());
  EXPECT_FALSE(window_.rotate_cw_action()->statusTip().isEmpty());
  window_.rotate_current_page(1);  // 단축키로 와도 무시
  QApplication::processEvents();
  EXPECT_FALSE(window_.isWindowModified());
  ASSERT_TRUE(open("simple.pdf"));
  EXPECT_TRUE(window_.rotate_cw_action()->isEnabled());
}

// 쪽 회전은 "쪽 조립" 권한(R≥3 비트 11)으로 판정한다 — 내용 수정 비트(4)와 무관 (#31 리뷰 W1)
TEST_F(EditTest, RotationFollowsAssemblePermission) {
  ASSERT_TRUE(open("perm-assemble-only.pdf"));  // 내용 수정 금지·조립 허용
  EXPECT_TRUE(window_.rotate_cw_action()->isEnabled());
  ASSERT_TRUE(open("perm-no-assemble.pdf"));  // 내용 수정 허용·조립 금지
  EXPECT_FALSE(window_.rotate_cw_action()->isEnabled());
  EXPECT_TRUE(window_.rotate_cw_action()->statusTip().contains(QStringLiteral("쪽 편집")));
}

// 읽기 전용 파일에 저장하면 원본을 그대로 두고 "다른 이름으로 저장" 을 안내한다 (#31 리뷰 C1)
TEST_F(EditTest, SavingReadOnlyFileExplainsAndKeepsOriginal) {
  const auto copy = open_copy("simple.pdf");
  const auto original = read_bytes(copy);
  namespace fs = std::filesystem;
  fs::permissions(copy, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read,
                  fs::perm_options::replace);
  struct Restore {
    fs::path path;
    ~Restore() {
      std::error_code ec;
      fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::add, ec);
    }
  } restore{copy};
  if (std::ofstream probe(copy, std::ios::binary | std::ios::app); probe) {
    GTEST_SKIP() << "running with privileges that can write read-only files";
  }
  ASSERT_TRUE(rotate(1));
  QString message;
  auto* poll = new QTimer();
  poll->setInterval(10);
  QObject::connect(poll, &QTimer::timeout, poll, [poll, &message] {
    auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
    if (box == nullptr || box->objectName() != QStringLiteral("save_failed")) return;
    message = box->text();
    box->accept();
    poll->deleteLater();
  });
  poll->start();
  window_.save_action()->trigger();
  ASSERT_TRUE(spin_until([&] { return !message.isEmpty() && !window_.saving(); }));
  EXPECT_TRUE(message.contains(QStringLiteral("다른 이름으로 저장"))) << message.toStdString();
  EXPECT_TRUE(window_.isWindowModified());
  EXPECT_EQ(read_bytes(copy), original);
}

// 편집이 워커에서 반영되는 사이에 저장하면 조용히 멈추지 않고 상태 표시줄로 알린다 (#31 리뷰 I1)
TEST_F(EditTest, SaveWhileEditBusyIsRefusedWithHint) {
  const auto copy = open_copy("simple.pdf");
  window_.rotate_current_page(1);
  ASSERT_TRUE(window_.edit_controller()->busy());
  EXPECT_FALSE(window_.save_to(QString::fromStdString(onpdf::path_to_utf8(copy))));
  EXPECT_FALSE(window_.saving());
  EXPECT_TRUE(window_.statusBar()->currentMessage().contains(QStringLiteral("잠시 후")))
      << window_.statusBar()->currentMessage().toStdString();
  ASSERT_TRUE(spin_until([&] { return idle(); }));
  // 반영이 끝나면 같은 저장이 된다
  ASSERT_TRUE(window_.save_to(QString::fromStdString(onpdf::path_to_utf8(copy))));
  ASSERT_TRUE(spin_until([&] { return !window_.saving(); }));
  EXPECT_FALSE(window_.isWindowModified());
}

// 편집이 반영되기 전에는 인쇄를 시작하지 않는다: 이전 쪽 크기로 배치를 계산할 수 있다 (#31 리뷰 I6)
TEST_F(EditTest, PrintWhileEditBusyIsRefused) {
  ASSERT_TRUE(open("simple.pdf"));
  QPrinter printer(QPrinter::HighResolution);
  printer.setOutputFormat(QPrinter::PdfFormat);
  printer.setOutputFileName(QString::fromStdString(onpdf::path_to_utf8(dir_ / "out.pdf")));
  window_.rotate_current_page(1);
  ASSERT_TRUE(window_.edit_controller()->busy());
  EXPECT_FALSE(window_.print_with(printer));
  EXPECT_EQ(window_.print_job(), nullptr);
  EXPECT_TRUE(window_.statusBar()->currentMessage().contains(QStringLiteral("잠시 후")))
      << window_.statusBar()->currentMessage().toStdString();
  ASSERT_TRUE(spin_until([&] { return idle(); }));
  ASSERT_TRUE(window_.print_with(printer));
  ASSERT_TRUE(spin_until([&] { return window_.print_job() != nullptr && !window_.print_job()->active(); }));
  EXPECT_TRUE(std::filesystem::exists(dir_ / "out.pdf"));
}

// 폴더에 새 파일을 만들 수 없으면 "다른 폴더에" 저장하라고 안내한다 (#31 리뷰 I4). 보호가 없는 환경(Windows·root)은 건너뛴다
TEST_F(EditTest, SavingIntoReadOnlyFolderExplains) {
  namespace fs = std::filesystem;
  const auto folder = dir_ / "locked";
  fs::create_directory(folder);
  const auto copy = folder / "simple.pdf";
  fs::copy_file(testdata("simple.pdf"), copy);
  ASSERT_TRUE(open_path(copy));
  const auto original = read_bytes(copy);
  fs::permissions(folder, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
  struct Restore {
    fs::path path;
    ~Restore() {
      std::error_code ec;
      fs::permissions(path, fs::perms::owner_all, fs::perm_options::add, ec);
    }
  } restore{folder};
  {
    const auto probe_path = folder / "probe.tmp";
    std::ofstream probe(probe_path, std::ios::binary);
    if (probe) {
      probe.close();
      std::error_code ec;
      fs::remove(probe_path, ec);
      GTEST_SKIP() << "folder write protection is not enforced here";
    }
  }
  ASSERT_TRUE(rotate(1));
  QString message;
  auto* poll = new QTimer();
  poll->setInterval(10);
  QObject::connect(poll, &QTimer::timeout, poll, [poll, &message] {
    auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
    if (box == nullptr || box->objectName() != QStringLiteral("save_failed")) return;
    message = box->text();
    box->accept();
    poll->deleteLater();
  });
  poll->start();
  window_.save_action()->trigger();
  ASSERT_TRUE(spin_until([&] { return !message.isEmpty() && !window_.saving(); }));
  EXPECT_TRUE(message.contains(QStringLiteral("폴더"))) << message.toStdString();
  EXPECT_TRUE(message.contains(QStringLiteral("다른 이름으로 저장"))) << message.toStdString();
  EXPECT_TRUE(window_.isWindowModified());
  EXPECT_EQ(read_bytes(copy), original);
}

TEST_F(EditTest, SignedDocumentSavesIncrementally) {
  const auto copy = open_copy("signed-dummy.pdf");
  const auto original = read_bytes(copy);
  ASSERT_TRUE(rotate(1));
  window_.save_action()->trigger();
  ASSERT_TRUE(spin_until([&] { return !window_.saving() && !window_.isWindowModified(); }));
  const auto after = read_bytes(copy);
  ASSERT_GT(after.size(), original.size());
  EXPECT_TRUE(std::equal(original.begin(), original.end(), after.begin())) << "signed bytes must be preserved";
}

}  // namespace
}  // namespace onpdf::app
