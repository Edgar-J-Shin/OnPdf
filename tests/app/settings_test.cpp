// 설정 저장·설정 화면 (#129): Settings 왕복·clamp, 편집 기본값 ↔ 툴바 양방향, 창 상태 복원, 최근
// 파일·마지막 폴더, 대화상자(확인·취소·기본값·언어 재시작 안내), 시작 배율, 고급(위험 안내·멈춤
// 초·캐시). 진입점이 임시 INI 로 격리하므로 개발자 설정은 건드리지 않는다

#include "settings.h"

#include "onpdf/core/platform/path.h"

#include "../test_support.h"
#include "dialog_helpers.h"
#include "document_view.h"
#include "i18n.h"
#include "main_window.h"
#include "settings_dialog.h"
#include "ui_dispatcher.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

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

class SettingsTest : public ::testing::Test {
 protected:
  SettingsTest() : worker_(options()) { Settings().reset_all(); }
  ~SettingsTest() override {
    window_.reset();
    Settings().reset_all();  // 뒤에 도는 다른 테스트 파일이 이 테스트의 창 상태를 받지 않게
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
  bool open(const char* name) { return open_path(testdata(name)); }
  std::filesystem::path copy_of(const char* name, const char* as = nullptr) {
    const auto copy = dir_ / (as != nullptr ? as : name);
    std::filesystem::copy_file(testdata(name), copy);
    return copy;
  }
  bool idle() { return !window_->edit_controller()->busy() && !window_->saving(); }
  bool wait_annotations(int page, std::size_t n) {
    return spin_until([&] {
      return idle() && view()->annotations(page) != nullptr &&
             view()->annotations(page)->size() == n;
    });
  }
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
  // 최근 파일 메뉴의 파일 항목 (data 에 경로)
  QStringList recent_entries() {
    QMenu* menu = window_->recent_menu();
    emit menu->aboutToShow();
    QStringList out;
    for (QAction* a : menu->actions()) {
      if (!a->data().isNull()) out << a->data().toString();
    }
    return out;
  }
  static QAction* action_with_data(QMenu* menu, const QVariant& data) {
    for (QAction* a : menu->actions()) {
      if (a->data() == data) return a;
    }
    return nullptr;
  }
  template <class W>
  static W* child(QWidget* dialog, const char* name) {
    W* w = dialog->findChild<W*>(QString::fromLatin1(name));
    EXPECT_NE(w, nullptr) << name;
    return w;
  }
  static void click_button(QWidget* dialog, QDialogButtonBox::StandardButton which) {
    auto* box = child<QDialogButtonBox>(dialog, "settings_buttons");
    ASSERT_NE(box, nullptr);
    box->button(which)->click();
  }

  onpdf::test::ScopedTempDir dir_;
  UiDispatcher dispatcher_;
  engine::PdfiumWorker worker_;
  std::unique_ptr<MainWindow> window_;
};

TEST_F(SettingsTest, RoundTripAndClamp) {
  {
    Settings s;
    s.set_border_width(4.0);
    s.set_opacity_percent(50);
    s.set_annotation_color({255, 0, 0, 255});
    s.set_text_size(48);
    s.set_text_color({0, 150, 255, 255});
    s.set_author(QStringLiteral("  Edgar  "));
    s.set_cache_mb(9999);
    s.set_stall_seconds(1);
    s.set_start_zoom(StartZoom::kFitPage);
    s.set_sidebar_start(SidebarStart::kHidden);
    s.set_language(LanguageChoice::kEnglish);
    s.sync();
  }
  Settings s;
  EXPECT_EQ(s.border_width(), 4.0);
  EXPECT_EQ(s.opacity_percent(), 50);
  EXPECT_EQ(s.opacity_alpha(), 128);
  EXPECT_EQ(s.annotation_color(), (engine::Color{255, 0, 0, 255}));
  EXPECT_EQ(s.text_size(), 48);
  EXPECT_EQ(s.text_color(), (engine::Color{0, 150, 255, 255}));
  EXPECT_EQ(s.author(), QStringLiteral("Edgar"));
  EXPECT_EQ(s.cache_mb(), Settings::kMaxCacheMb);
  EXPECT_EQ(s.stall_seconds(), Settings::kMinStallSeconds);
  EXPECT_EQ(s.start_zoom(), StartZoom::kFitPage);
  EXPECT_EQ(s.sidebar_start(), SidebarStart::kHidden);
  EXPECT_EQ(s.language(), LanguageChoice::kEnglish);
  // 허용 목록 밖은 가까운 값으로, 범위 밖·쓰레기는 기본값으로
  s.set_border_width(3.0);
  EXPECT_EQ(s.border_width(), 2.0);
  s.set_text_size(1000);
  EXPECT_EQ(s.text_size(), Settings::kMaxTextSize);  // clamp (#141: 직접 입력, snap 없음)
  s.set_text_size(10.5);
  EXPECT_EQ(s.text_size(), 10.5);
  s.reset_preferences();
  EXPECT_EQ(s.border_width(), Settings::kDefaultBorderWidth);
  EXPECT_EQ(s.language(), LanguageChoice::kSystem);

  // 손상된 파일: 기본값으로 동작하고 죽지 않는다
  const QString saved = Settings::default_path();
  const QString garbage = qpath(dir_ / "garbage.ini");
  {
    QFile f(garbage);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("\x00\xff[[[not ini\n=\n", 16);
  }
  Settings::set_default_path(garbage);
  {
    Settings g;
    EXPECT_EQ(g.border_width(), Settings::kDefaultBorderWidth);
    EXPECT_EQ(g.cache_mb(), Settings::kDefaultCacheMb);
    EXPECT_TRUE(g.recent_files().isEmpty());
  }
  Settings::set_default_path(saved);
}

TEST_F(SettingsTest, EditDefaultsApplyToWindowAndNewShapes) {
  {
    Settings s;
    s.set_border_width(4.0);
    s.set_opacity_percent(50);
    s.set_text_size(48);
    s.set_annotation_color({255, 0, 0, 255});
  }
  MainWindow& w = make_window();
  EXPECT_EQ(w.shape_border_width(), 4.0);
  EXPECT_EQ(w.shape_alpha(), 128);
  EXPECT_EQ(w.annotation_color(), (engine::Color{255, 0, 0, 255}));
  EXPECT_EQ(w.text_size_combo()->currentData().toInt(), 48);
  EXPECT_TRUE(w.width_button()->text().contains(QStringLiteral("4")));

  ASSERT_TRUE(open("simple.pdf"));
  w.tool_action(AnnotationTool::kSquare)->trigger();
  drag(view()->page_point_to_viewport(0, {100, 400}),
       view()->page_point_to_viewport(0, {300, 500}));
  ASSERT_TRUE(wait_annotations(0, 1));
  const auto& a = view()->annotations(0)->front();
  EXPECT_NEAR(a.border_width, 4.0f, 0.01f);
  EXPECT_NEAR(a.opacity, 0.5f, 0.01f);
  if (a.has_color) {  // AP 가 만들어진 뒤에는 색을 읽지 못한다 (#35)
    EXPECT_EQ(a.color.r, 255);
    EXPECT_EQ(a.color.g, 0);
  }
}

TEST_F(SettingsTest, ToolbarChangesPersist) {
  MainWindow& w = make_window();
  QAction* eight = action_with_data(w.width_button()->menu(), 8.0);
  ASSERT_NE(eight, nullptr);
  eight->trigger();
  EXPECT_EQ(w.shape_border_width(), 8.0);
  EXPECT_EQ(Settings().border_width(), 8.0);
  QAction* quarter = action_with_data(w.opacity_button()->menu(), 25);
  ASSERT_NE(quarter, nullptr);
  quarter->trigger();
  EXPECT_EQ(Settings().opacity_percent(), 25);
  const int index = w.text_size_combo()->findData(12);
  ASSERT_GE(index, 0);
  w.text_size_combo()->setCurrentIndex(index);
  EXPECT_EQ(Settings().text_size(), 12);
}

TEST_F(SettingsTest, WindowStateRestored) {
  {
    MainWindow& w = make_window();
    w.resize(640, 480);
    ASSERT_TRUE(w.sidebar_action()->isChecked());
    w.sidebar_action()->trigger();  // 숨김
    EXPECT_FALSE(w.sidebar()->isVisible());
    w.close();
    QApplication::processEvents();
    window_.reset();
  }
  EXPECT_FALSE(Settings().sidebar_visible());
  MainWindow& again = make_window();
  EXPECT_EQ(again.size(), QSize(640, 480));
  EXPECT_FALSE(again.sidebar_action()->isChecked());
  EXPECT_FALSE(again.sidebar()->isVisible());
  window_.reset();

  // "항상 보임" 이면 마지막 상태와 무관하게 보인다. 저장된 창 상태가 없으면 기본 크기
  Settings().reset_all();
  Settings().set_sidebar_start(SidebarStart::kVisible);
  MainWindow& fresh = make_window();
  EXPECT_EQ(fresh.size(), QSize(900, 1000));
  EXPECT_TRUE(fresh.sidebar_action()->isChecked());
}

TEST_F(SettingsTest, RecentFilesMenu) {
  MainWindow& w = make_window();
  EXPECT_TRUE(recent_entries().isEmpty());
  const auto a = copy_of("simple.pdf", "a.pdf");
  const auto b = copy_of("simple.pdf", "b.pdf");
  ASSERT_TRUE(open_path(a));
  ASSERT_TRUE(open_path(b));
  QStringList entries = recent_entries();
  ASSERT_EQ(entries.size(), 2);
  EXPECT_EQ(entries[0], QFileInfo(qpath(b)).absoluteFilePath());
  EXPECT_EQ(entries[1], QFileInfo(qpath(a)).absoluteFilePath());
  // 같은 파일을 다시 열면 맨 위로만 간다
  ASSERT_TRUE(open_path(a));
  entries = recent_entries();
  ASSERT_EQ(entries.size(), 2);
  EXPECT_EQ(entries[0], QFileInfo(qpath(a)).absoluteFilePath());
  // 없어진 파일은 메뉴에서 빠진다
  std::filesystem::remove(b);
  entries = recent_entries();
  ASSERT_EQ(entries.size(), 1);
  // 항목을 고르면 그 파일을 연다
  ASSERT_TRUE(open("simple.pdf"));
  QAction* item = action_with_data(w.recent_menu(), QFileInfo(qpath(a)).absoluteFilePath());
  ASSERT_NE(item, nullptr);
  item->trigger();
  EXPECT_TRUE(spin_until([&] { return w.windowTitle().contains(QStringLiteral("a.pdf")); }));
  // 상한 10
  for (int i = 0; i < 12; ++i)
    Settings().add_recent(qpath(dir_.path() / ("x" + std::to_string(i))));
  EXPECT_EQ(Settings().recent_files().size(), 0);  // 없는 파일은 걸러진다
  for (int i = 0; i < 12; ++i) {
    const std::string name = "r" + std::to_string(i) + ".pdf";
    Settings().add_recent(qpath(copy_of("simple.pdf", name.c_str())));
  }
  EXPECT_EQ(Settings().recent_files().size(), Settings::kMaxRecent);
  // 비우기
  QMenu* menu = w.recent_menu();
  emit menu->aboutToShow();
  QAction* clear = menu->findChild<QAction*>(QStringLiteral("recent_clear"));
  ASSERT_NE(clear, nullptr);
  clear->trigger();
  EXPECT_TRUE(recent_entries().isEmpty());
}

TEST_F(SettingsTest, LastDirRemembered) {
  MainWindow& w = make_window();
  EXPECT_TRUE(Settings().last_dir(DirKind::kOpen).isEmpty());
  const auto copy = copy_of("simple.pdf");
  ASSERT_TRUE(open_path(copy));
  const QString dir = QFileInfo(qpath(copy)).absolutePath();
  EXPECT_EQ(Settings().last_dir(DirKind::kOpen), dir);
  const auto saved = dir_ / "saved.pdf";
  ASSERT_TRUE(w.save_to(qpath(saved)));
  ASSERT_TRUE(spin_until([&] { return idle() && std::filesystem::exists(saved); }));
  EXPECT_EQ(Settings().last_dir(DirKind::kSave), dir);
  EXPECT_EQ(Settings().recent_files().first(), QFileInfo(qpath(saved)).absoluteFilePath());
}

TEST_F(SettingsTest, DialogApplyCancelDefaultsAndLanguageNotice) {
  MainWindow& w = make_window();
  // 두께 8 → 확인
  when_dialog("settings_dialog", [](QWidget* d) {
    auto* width = child<QComboBox>(d, "settings_border_width");
    width->setCurrentIndex(width->findData(8.0));
    click_button(d, QDialogButtonBox::Ok);
  });
  w.open_preferences();
  EXPECT_EQ(w.shape_border_width(), 8.0);
  EXPECT_EQ(Settings().border_width(), 8.0);
  // 불투명도 25 → 취소: 불변
  when_dialog("settings_dialog", [](QWidget* d) {
    auto* opacity = child<QComboBox>(d, "settings_opacity");
    opacity->setCurrentIndex(opacity->findData(25));
    click_button(d, QDialogButtonBox::Cancel);
  });
  w.open_preferences();
  EXPECT_EQ(w.shape_alpha(), 255);
  EXPECT_EQ(Settings().opacity_percent(), 100);
  // 기본값으로 → 확인: 두께 2
  when_dialog("settings_dialog", [](QWidget* d) {
    click_button(d, QDialogButtonBox::RestoreDefaults);
    click_button(d, QDialogButtonBox::Ok);
  });
  w.open_preferences();
  EXPECT_EQ(w.shape_border_width(), 2.0);
  // 작성자·언어 → 확인: 재시작 안내, 실행 중 언어는 그대로
  when_dialog("settings_dialog", [](QWidget* d) {
    child<QLineEdit>(d, "settings_author")->setText(QStringLiteral("Tester"));
    auto* lang = child<QComboBox>(d, "settings_language");
    lang->setCurrentIndex(lang->findData(static_cast<int>(LanguageChoice::kEnglish)));
    click_button(d, QDialogButtonBox::Ok);
  });
  w.open_preferences();
  EXPECT_TRUE(spin_until([] { return find_dialog("language_restart_notice") != nullptr; }, 5000));
  EXPECT_EQ(Settings().language(), LanguageChoice::kEnglish);
  EXPECT_EQ(Settings().author(), QStringLiteral("Tester"));
  EXPECT_EQ(current_language(), Language::kKorean);
  if (QWidget* notice = find_dialog("language_restart_notice")) notice->close();
  // 작성자는 새 주석의 /T 로 들어간다
  ASSERT_TRUE(open("simple.pdf"));
  w.tool_action(AnnotationTool::kSquare)->trigger();
  drag(view()->page_point_to_viewport(0, {100, 400}),
       view()->page_point_to_viewport(0, {300, 500}));
  ASSERT_TRUE(wait_annotations(0, 1));
  EXPECT_EQ(view()->annotations(0)->front().author, "Tester");
}

TEST_F(SettingsTest, StartZoomApplied) {
  Settings().set_start_zoom(StartZoom::kFitPage);
  make_window();
  ASSERT_TRUE(open("simple.pdf"));
  EXPECT_EQ(view()->zoom_mode(), ZoomMode::kFitPage);
  // 마지막 값: 2.0 배로 두고 닫은 상태
  Settings().set_start_zoom(StartZoom::kLast);
  Settings().set_last_zoom(ZoomKind::kCustom, 2.0);
  ASSERT_TRUE(open("korean-embedded.pdf"));
  EXPECT_EQ(view()->zoom_mode(), ZoomMode::kCustom);
  EXPECT_DOUBLE_EQ(view()->zoom(), 2.0);
  // 실제 크기
  Settings().set_start_zoom(StartZoom::kActualSize);
  ASSERT_TRUE(open("simple.pdf"));
  EXPECT_DOUBLE_EQ(view()->zoom(), 1.0);
}

TEST_F(SettingsTest, AdvancedOptions) {
  Settings().set_hazard_notice(false);
  Settings().set_stall_seconds(10);
  Settings().set_cache_mb(64);
  MainWindow& w = make_window();
  EXPECT_EQ(w.stall_threshold(), 10s);
  EXPECT_EQ(view()->cache_limit(), std::size_t{64} << 20);
  ASSERT_TRUE(open("pattern-bomb.pdf"));
  EXPECT_FALSE(spin_until([] { return find_dialog("render_hazards_notice") != nullptr; }, 1000));
  // 켜면 안내가 뜬다
  Settings().set_hazard_notice(true);
  ASSERT_TRUE(open("simple.pdf"));
  ASSERT_TRUE(open("pattern-bomb.pdf"));
  EXPECT_TRUE(spin_until([] { return find_dialog("render_hazards_notice") != nullptr; }, 5000));
  if (QWidget* notice = find_dialog("render_hazards_notice")) notice->close();
  // 대화상자에서 캐시 상한을 바꾸면 바로 적용
  when_dialog("settings_dialog", [](QWidget* d) {
    child<QSpinBox>(d, "settings_cache_mb")->setValue(128);
    child<QSpinBox>(d, "settings_stall_seconds")->setValue(120);
    click_button(d, QDialogButtonBox::Ok);
  });
  w.open_preferences();
  EXPECT_EQ(view()->cache_limit(), std::size_t{128} << 20);
  EXPECT_EQ(w.stall_threshold(), 120s);
}

}  // namespace
}  // namespace onpdf::app
