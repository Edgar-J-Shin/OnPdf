// 충돌 보고 (#131): 세션 표식 수명, 다음 실행 안내와 선택, 끄기, 강제 종료 표식, 기록 정리, 이슈
// URL, 기록 형식·읽기. 크래시 자체는 재현하지 않는다(테스트 프로세스는 핸들러를 설치하지 않는다).
// 보고 폴더는 임시 폴더로 바꿔 쓴다

#include "crash_report.h"

#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/platform/path.h"

#include "../test_support.h"
#include "crash_handler.h"
#include "dialog_helpers.h"
#include "document_view.h"
#include "main_window.h"
#include "settings.h"
#include "ui_dispatcher.h"
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <latch>
#include <memory>
#include <string>

namespace onpdf::app {
namespace {

using namespace std::chrono_literals;
using onpdf::test::find_dialog;
using onpdf::test::testdata;
using onpdf::test::when_dialog;

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

// 워커를 막는 렌더 작업 (stall_test 와 같은 방식)
class Block {
 public:
  Block(engine::PdfiumWorker& worker, int page)
      : entered_(std::make_shared<std::latch>(1)), release_(std::make_shared<std::latch>(1)) {
    worker.post(
        [entered = entered_, release = release_](bool) {
          entered->count_down();
          release->wait();
        },
        engine::Priority::kNormal, engine::TaskKind::kRender, page);
    entered_->wait();
  }
  ~Block() {
    if (!opened_) release_->count_down();
  }

 private:
  std::shared_ptr<std::latch> entered_;
  std::shared_ptr<std::latch> release_;
  bool opened_ = false;
};

class CrashReportTest : public ::testing::Test {
 protected:
  CrashReportTest() : worker_(options()) {
    Settings().reset_all();
    reports_ = dir_ / "reports";
    std::filesystem::create_directories(reports_);
    set_report_dir_override(qpath(reports_));
  }
  ~CrashReportTest() override {
    window_.reset();
    set_report_dir_override(QString());
    Settings().reset_all();
  }
  static engine::PdfiumWorker::Options options() {
    engine::PdfiumWorker::Options o;
    o.use_system_fonts = false;
    return o;
  }
  MainWindow& make_window() {
    window_ = std::make_unique<MainWindow>(worker_, dispatcher_);
    window_->show();
    return *window_;
  }
  DocumentView* view() { return window_->document_view(); }
  bool open_path(const std::filesystem::path& path) {
    const QString file = qpath(path.filename());
    window_->open_file(qpath(path));
    return spin_until(
        [&] { return window_->windowTitle().contains(file) && view()->has_document(); });
  }
  std::filesystem::path copy_of(const char* name, const char* as = nullptr) {
    const auto copy = dir_ / (as != nullptr ? as : name);
    std::filesystem::copy_file(testdata(name), copy);
    return copy;
  }
  bool idle() { return !window_->edit_controller()->busy() && !window_->saving(); }
  // 표식을 미리 남긴다 (지난 실행이 죽은 것처럼)
  SessionMarker plant_marker(const QString& path, const QString& stage) {
    SessionMarker m;
    m.path = path;
    m.stage = stage;
    m.since = QDateTime::currentDateTime().addSecs(-60);
    m.last_action = QStringLiteral("render p.2");
    m.version = app_version();
    Settings().set_session_marker(m);
    return m;
  }
  void write_record(long long epoch, const std::string& body) {
    QFile f(qpath(reports_ / ("crash-" + std::to_string(epoch) + ".txt")));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(body.c_str());
  }
  static QAbstractButton* button_with_role(QMessageBox* box, QMessageBox::ButtonRole role) {
    for (QAbstractButton* b : box->buttons()) {
      if (box->buttonRole(b) == role) return b;
    }
    return nullptr;
  }

  onpdf::test::ScopedTempDir dir_;
  std::filesystem::path reports_;
  UiDispatcher dispatcher_;
  engine::PdfiumWorker worker_;
  std::unique_ptr<MainWindow> window_;
};

TEST_F(CrashReportTest, MarkerLifecycle) {
  MainWindow& w = make_window();
  EXPECT_FALSE(Settings().session_marker().valid());
  const auto copy = copy_of("simple.pdf");
  ASSERT_TRUE(open_path(copy));
  SessionMarker m = Settings().session_marker();
  EXPECT_TRUE(m.valid());
  EXPECT_EQ(m.stage, QStringLiteral("viewing"));
  EXPECT_EQ(m.path, QFileInfo(qpath(copy)).absoluteFilePath());
  EXPECT_EQ(m.last_action, QStringLiteral("view"));
  EXPECT_EQ(m.version, app_version());
  view()->go_to_page(1);
  EXPECT_TRUE(spin_until(
      [] { return Settings().session_marker().last_action == QStringLiteral("render p.2"); }));
  const auto saved = dir_ / "saved.pdf";
  ASSERT_TRUE(w.save_to(qpath(saved)));
  EXPECT_EQ(Settings().session_marker().last_action, QStringLiteral("save"));
  ASSERT_TRUE(spin_until([&] { return idle() && std::filesystem::exists(saved); }));
  // 정상 종료 → 표식 없음
  w.close();
  QApplication::processEvents();
  EXPECT_FALSE(Settings().session_marker().valid());
  EXPECT_TRUE(Settings().session_marker().path.isEmpty());
  window_.reset();

  // 열기 실패 → 표식 없음 ("열 수 없습니다" 모달 상자는 자동으로 닫는다)
  make_window();
  QTimer closer;
  closer.setInterval(10);
  QObject::connect(&closer, &QTimer::timeout, &closer, [] {
    if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) box->accept();
  });
  closer.start();
  window_->open_file(qpath(testdata("truncated.pdf")));
  EXPECT_TRUE(spin_until([] { return !Settings().session_marker().valid(); }, 10000));
}

TEST_F(CrashReportTest, NoticeShowsRecordAndHonoursChoice) {
  const auto copy = copy_of("simple.pdf");
  const SessionMarker m =
      plant_marker(QFileInfo(qpath(copy)).absoluteFilePath(), QStringLiteral("viewing"));
  write_record(
      QDateTime::currentSecsSinceEpoch() - 30,
      "version=0.0.0\npdfium=chromium/0\nsignal=SIGTRAP(5)\nlast_action=render p.2\ntime=" +
          std::to_string(QDateTime::currentSecsSinceEpoch() - 30) + "\n");
  MainWindow& w = make_window();
  QString shown;
  when_dialog("crash_notice", [&shown](QWidget* d) {
    auto* box = qobject_cast<QMessageBox*>(d);
    ASSERT_NE(box, nullptr);
    shown = box->text();
    EXPECT_TRUE(box->isModal());
    EXPECT_NE(box->findChild<QPushButton*>(QStringLiteral("crash_notice_open")), nullptr);
    box->findChild<QPushButton*>(QStringLiteral("crash_notice_leave"))->click();
  });
  EXPECT_TRUE(w.maybe_show_crash_notice(QString()));
  EXPECT_TRUE(shown.contains(QStringLiteral("simple.pdf"))) << shown.toStdString();
  EXPECT_TRUE(shown.contains(QStringLiteral("SIGTRAP"))) << shown.toStdString();
  EXPECT_FALSE(view()->has_document());
  EXPECT_FALSE(Settings().session_marker().valid());

  // 다시 열기 → 문서가 열린다
  plant_marker(m.path, QStringLiteral("opening"));
  when_dialog("crash_notice", [](QWidget* d) {
    d->findChild<QPushButton*>(QStringLiteral("crash_notice_open"))->click();
  });
  EXPECT_TRUE(w.maybe_show_crash_notice(QString()));
  EXPECT_TRUE(spin_until([&] { return view()->has_document(); }));
  EXPECT_EQ(Settings().session_marker().stage, QStringLiteral("viewing"));
  w.close();
  QApplication::processEvents();
  window_.reset();

  // 명령줄로 같은 파일을 넘기고 "열지 않기" → false
  plant_marker(m.path, QStringLiteral("forced-quit"));
  MainWindow& again = make_window();
  when_dialog("crash_notice", [](QWidget* d) {
    d->findChild<QPushButton*>(QStringLiteral("crash_notice_leave"))->click();
  });
  EXPECT_FALSE(again.maybe_show_crash_notice(qpath(copy)));
  // 다른 파일이면 true
  plant_marker(m.path, QStringLiteral("viewing"));
  when_dialog("crash_notice", [](QWidget* d) {
    d->findChild<QPushButton*>(QStringLiteral("crash_notice_leave"))->click();
  });
  EXPECT_TRUE(again.maybe_show_crash_notice(qpath(testdata("korean-embedded.pdf"))));
  // 표식의 파일이 사라졌으면 "다시 열기" 버튼이 없다
  plant_marker(qpath(dir_ / "gone.pdf"), QStringLiteral("opening"));
  bool had_open = true;
  when_dialog("crash_notice", [&had_open](QWidget* d) {
    had_open = d->findChild<QPushButton*>(QStringLiteral("crash_notice_open")) != nullptr;
    d->findChild<QPushButton*>(QStringLiteral("crash_notice_leave"))->click();
  });
  EXPECT_TRUE(again.maybe_show_crash_notice(QString()));
  EXPECT_FALSE(had_open);
}

TEST_F(CrashReportTest, NoticeDisabledBySettingOrCheckbox) {
  const auto copy = copy_of("simple.pdf");
  const QString path = QFileInfo(qpath(copy)).absoluteFilePath();
  plant_marker(path, QStringLiteral("viewing"));
  MainWindow& w = make_window();
  when_dialog("crash_notice", [](QWidget* d) {
    d->findChild<QCheckBox*>(QStringLiteral("crash_notice_dont_show"))->setChecked(true);
    d->findChild<QPushButton*>(QStringLiteral("crash_notice_leave"))->click();
  });
  EXPECT_TRUE(w.maybe_show_crash_notice(QString()));
  EXPECT_FALSE(Settings().crash_notice());
  // 꺼진 상태: 안내 없이 상태 표시줄 한 줄, 표식은 지운다
  plant_marker(path, QStringLiteral("viewing"));
  QStringList messages;
  QObject::connect(w.statusBar(), &QStatusBar::messageChanged, &w,
                   [&messages](const QString& m) { messages << m; });
  EXPECT_TRUE(w.maybe_show_crash_notice(QString()));
  EXPECT_EQ(find_dialog("crash_notice"), nullptr);
  EXPECT_FALSE(Settings().session_marker().valid());
  EXPECT_TRUE(std::any_of(messages.begin(), messages.end(), [](const QString& m) {
    return m.contains(QStringLiteral("simple.pdf"));
  })) << messages.join(QStringLiteral(" | ")).toStdString();
}

TEST_F(CrashReportTest, ForcedQuitLeavesMarker) {
  MainWindow& w = make_window();
  w.set_stall_threshold(200ms);
  int exit_code = -1;
  w.set_force_exit_hook([&exit_code](int code) {
    exit_code = code;
    EXPECT_EQ(Settings().session_marker().stage, QStringLiteral("forced-quit"));  // _Exit 전에
  });
  ASSERT_TRUE(open_path(copy_of("simple.pdf")));
  {
    Block block(worker_, 3);
    ASSERT_TRUE(spin_until([] { return find_dialog("worker_stalled") != nullptr; }, 5000));
    auto* box = qobject_cast<QMessageBox*>(find_dialog("worker_stalled"));
    ASSERT_NE(box, nullptr);
    button_with_role(box, QMessageBox::DestructiveRole)->click();
  }
  EXPECT_EQ(exit_code, 3);
  const SessionMarker m = Settings().session_marker();
  EXPECT_EQ(m.stage, QStringLiteral("forced-quit"));
  EXPECT_EQ(m.last_action, QStringLiteral("render p.4"));
}

TEST_F(CrashReportTest, PruneKeepsNewestGroups) {
  for (long long e = 100; e < 108; ++e) {
    write_record(e, "signal=SIGSEGV(11)\n");
    if (e % 2 == 0) {
      QFile dmp(qpath(reports_ / ("crash-" + std::to_string(e) + ".dmp")));
      ASSERT_TRUE(dmp.open(QIODevice::WriteOnly));
      dmp.write("x", 1);
    }
  }
  QFile other(qpath(reports_ / "notes.txt"));
  ASSERT_TRUE(other.open(QIODevice::WriteOnly));
  other.close();
  const int removed = crash::prune_reports(reports_, 5);
  EXPECT_EQ(removed, 5);  // 100·101·102 묶음 (100·102 는 txt+dmp)
  for (long long e = 100; e < 103; ++e)
    EXPECT_FALSE(std::filesystem::exists(reports_ / ("crash-" + std::to_string(e) + ".txt")));
  for (long long e = 103; e < 108; ++e)
    EXPECT_TRUE(std::filesystem::exists(reports_ / ("crash-" + std::to_string(e) + ".txt")));
  EXPECT_TRUE(std::filesystem::exists(reports_ / "crash-104.dmp"));
  EXPECT_TRUE(std::filesystem::exists(reports_ / "notes.txt"));
  EXPECT_EQ(crash::prune_reports(reports_, 5), 0);
}

TEST_F(CrashReportTest, IssueUrlHasVersionsAndNoPath) {
  SessionMarker m;
  m.path = QStringLiteral("/Users/someone/secret folder/report.pdf");
  m.stage = QStringLiteral("viewing");
  m.since = QDateTime::currentDateTime();
  m.last_action = QStringLiteral("render p.2");
  CrashRecord r;
  r.signal_text = QStringLiteral("SIGTRAP(5)");
  r.last_action = QStringLiteral("render p.3");
  const QUrl url = issue_url(m, r);
  EXPECT_EQ(url.host(), QStringLiteral("github.com"));
  EXPECT_EQ(url.path(), QStringLiteral("/Edgar-J-Shin/OnPdf/issues/new"));
  const QUrlQuery query(url);
  const QString title = query.queryItemValue(QStringLiteral("title"), QUrl::FullyDecoded);
  const QString body = query.queryItemValue(QStringLiteral("body"), QUrl::FullyDecoded);
  EXPECT_TRUE(title.contains(app_version()));
  EXPECT_TRUE(body.contains(app_version()));
  EXPECT_TRUE(body.contains(pdfium_version()));
  EXPECT_TRUE(body.contains(QStringLiteral("SIGTRAP(5)")));
  EXPECT_TRUE(body.contains(QStringLiteral("viewing")));
  EXPECT_TRUE(body.contains(QStringLiteral("render p.3")));
  EXPECT_FALSE(body.contains(QStringLiteral("secret")));
  EXPECT_FALSE(body.contains(QStringLiteral("report.pdf")));
  EXPECT_FALSE(title.contains(QStringLiteral("report.pdf")));
  // 기록이 없으면 unknown
  const QUrlQuery no_record(issue_url(m, std::nullopt));
  EXPECT_TRUE(no_record.queryItemValue(QStringLiteral("body"), QUrl::FullyDecoded)
                  .contains(QStringLiteral("unknown")));
}

TEST_F(CrashReportTest, FormatRecordIsStable) {
  char buf[256];
  crash::RecordFields f;
  f.version = "0.6.0";
  f.pdfium = "chromium/8179";
  f.is_exception = false;
  f.signal_or_code = 5;  // SIGTRAP
  f.last_action = "render p.2";
  f.epoch = 1764300000;
  const std::size_t n = crash::format_record(buf, sizeof buf, f);
  const std::string expected = std::string("version=0.6.0\npdfium=chromium/8179\nsignal=") +
                               crash::signal_name(5) +
                               "(5)\nlast_action=render p.2\ntime=1764300000\n";
  EXPECT_EQ(std::string(buf), expected);
  EXPECT_EQ(n, expected.size());
  f.is_exception = true;
  f.signal_or_code = static_cast<long>(0x80000003L);
  crash::format_record(buf, sizeof buf, f);
  EXPECT_NE(std::string(buf).find("exception=0x80000003\n"), std::string::npos);
  // 작은 버퍼: 잘리되 널 종료
  char small[16];
  const std::size_t m = crash::format_record(small, sizeof small, f);
  EXPECT_EQ(m, 15u);
  EXPECT_EQ(small[15], '\0');
  EXPECT_EQ(std::string(small), "version=0.6.0\np");
  // 기록 읽기와 왕복
  const auto parsed = parse_record(QString::fromUtf8(expected.c_str()), QDateTime());
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->signal_text, QString::fromLatin1(crash::signal_name(5)) + "(5)");
  EXPECT_EQ(parsed->last_action, QStringLiteral("render p.2"));
  EXPECT_EQ(parsed->time.toSecsSinceEpoch(), 1764300000);
  EXPECT_FALSE(parse_record(QStringLiteral("version=1\n"), QDateTime()).has_value());
}

TEST_F(CrashReportTest, LatestRecordSince) {
  const qint64 now = QDateTime::currentSecsSinceEpoch();
  write_record(now - 3600,
               "signal=SIGSEGV(11)\nlast_action=old\ntime=" + std::to_string(now - 3600) + "\n");
  write_record(now - 10,
               "signal=SIGTRAP(5)\nlast_action=new\ntime=" + std::to_string(now - 10) + "\n");
  const auto latest =
      latest_record_since(QDir(qpath(reports_)), QDateTime::fromSecsSinceEpoch(now - 600));
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->last_action, QStringLiteral("new"));
  EXPECT_EQ(latest->signal_text, QStringLiteral("SIGTRAP(5)"));
  const auto none =
      latest_record_since(QDir(qpath(reports_)), QDateTime::fromSecsSinceEpoch(now + 100));
  EXPECT_FALSE(none.has_value());
  const auto any = latest_record_since(QDir(qpath(reports_)), QDateTime());
  ASSERT_TRUE(any.has_value());
  EXPECT_EQ(any->last_action, QStringLiteral("new"));
}

}  // namespace
}  // namespace onpdf::app
