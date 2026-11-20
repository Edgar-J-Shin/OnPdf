// 폼 채우기 (#51, offscreen): 텍스트 칸 오버레이(확정·취소·Tab), 체크·라디오, 선택지 메뉴, 읽기 전용, 권한, 저장 전 확정
#include "document_view.h"
#include "form_field_editor.h"
#include "main_window.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/platform/path.h"
#include "ui_dispatcher.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QTimer>

#include <filesystem>

namespace onpdf::app {
namespace {

using onpdf::test::ScopedTempDir;
using onpdf::test::testdata;

template <class Pred>
bool spin_until(Pred pred, int timeout_ms = 10000) {
  QElapsedTimer timer;
  timer.start();
  while (!pred()) {
    if (timer.elapsed() > timeout_ms) return false;
    QApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  return true;
}

// QMenu::exec 가 도는 동안 팝업을 찾아 fn 을 부른다
template <class Fn>
void when_popup(const char* object_name, Fn fn) {
  auto* poll = new QTimer();
  poll->setInterval(10);
  QObject::connect(poll, &QTimer::timeout, poll, [poll, object_name, fn] {
    QWidget* w = QApplication::activePopupWidget();
    if (w == nullptr || w->objectName() != QLatin1String(object_name)) return;
    fn(static_cast<QMenu*>(w));
    poll->deleteLater();
  });
  poll->start();
}

class FormToolTest : public ::testing::Test {
 protected:
  FormToolTest() : worker_(options()), window_(worker_, dispatcher_) {
    window_.resize(900, 1000);
    window_.show();
  }
  static engine::PdfiumWorker::Options options() {
    engine::PdfiumWorker::Options o;
    o.use_system_fonts = false;
    return o;
  }
  DocumentView* view() { return window_.document_view(); }
  bool open_path(const std::filesystem::path& path) {
    const QString file = QString::fromStdString(onpdf::path_to_utf8(path.filename()));
    window_.open_file(QString::fromStdString(onpdf::path_to_utf8(path)));
    return spin_until([&] { return window_.windowTitle().contains(file) && view()->has_document(); });
  }
  std::filesystem::path open_copy(const char* name) {
    const auto copy = dir_ / name;
    if (!std::filesystem::exists(copy)) std::filesystem::copy_file(testdata(name), copy);
    EXPECT_TRUE(open_path(copy));
    return copy;
  }
  bool idle() { return !window_.edit_controller()->busy() && !window_.saving(); }
  std::optional<engine::FormWidgetInfo> field(int page, const std::string& name, int control = 0) {
    if (const auto* list = view()->form_widgets(page)) {
      for (const auto& w : *list) if (w.ref.name == name && w.ref.control_index == control) return w;
    }
    return std::nullopt;
  }
  bool wait_value(int page, const std::string& name, const std::string& value, int control = 0) {
    return spin_until([&] {
      const auto w = field(page, name, control);
      return idle() && w && w->value == value;
    });
  }
  bool wait_checked(int page, const std::string& name, int control, bool checked) {
    return spin_until([&] {
      const auto w = field(page, name, control);
      return idle() && w && w->checked == checked;
    });
  }
  QPointF center_of(int page, const std::string& name, int control = 0) {
    const auto w = field(page, name, control);
    EXPECT_TRUE(w) << name;
    return view()->page_rect_to_viewport(page, w ? w->rect : onpdf::Rect{}).center();
  }
  void click(QPointF at) {
    QMouseEvent press(QEvent::MouseButtonPress, at, view()->viewport()->mapToGlobal(at.toPoint()), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view()->viewport(), &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, view()->viewport()->mapToGlobal(at.toPoint()), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(view()->viewport(), &release);
  }
  void settle_clicks() {  // 연속 클릭 판정(두 번 클릭)을 피한다
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 600) QApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  QWidget* editor_input() {
    FormFieldEditor* editor = view()->form_editor();
    if (editor == nullptr) return nullptr;
    if (auto* line = editor->findChild<QLineEdit*>(QStringLiteral("form_text_line"))) return line;
    return editor->findChild<QPlainTextEdit*>(QStringLiteral("form_text_multi"));
  }
  void key(QWidget* target, Qt::Key k, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QKeyEvent ev(QEvent::KeyPress, k, mods);
    QApplication::sendEvent(target, &ev);
  }
  bool wait_widgets() { return spin_until([&] { return field(0, "name").has_value(); }); }

  ScopedTempDir dir_;
  UiDispatcher dispatcher_;
  engine::PdfiumWorker worker_;
  MainWindow window_;
};

TEST_F(FormToolTest, TextFieldOverlayCommitsAndUndoes) {
  open_copy("form-fields.pdf");
  ASSERT_TRUE(wait_widgets());
  EXPECT_TRUE(view()->form_fillable());
  click(center_of(0, "name"));
  ASSERT_NE(view()->form_editor(), nullptr);
  EXPECT_EQ(view()->form_editor()->text(), QStringLiteral("Hong")) << "현재 값이 채워진다";
  view()->form_editor()->set_text(QStringLiteral("홍길동"));
  key(editor_input(), Qt::Key_Return);
  EXPECT_EQ(view()->form_editor(), nullptr);
  ASSERT_TRUE(wait_value(0, "name", "홍길동"));
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("폼 입력")));
  EXPECT_TRUE(window_.isWindowModified());
  ASSERT_TRUE(window_.edit_controller()->undo());
  ASSERT_TRUE(wait_value(0, "name", "Hong"));
  // Esc: 취소 — 문서를 바꾸지 않는다
  settle_clicks();
  click(center_of(0, "name"));
  ASSERT_NE(view()->form_editor(), nullptr);
  view()->form_editor()->set_text(QStringLiteral("버림"));
  key(editor_input(), Qt::Key_Escape);
  EXPECT_EQ(view()->form_editor(), nullptr);
  settle_clicks();
  EXPECT_TRUE(idle());
  EXPECT_EQ(field(0, "name")->value, "Hong");
  EXPECT_FALSE(window_.edit_controller()->can_undo() && window_.undo_action()->text().contains(QStringLiteral("폼 입력")) &&
               field(0, "name")->value != "Hong");
}

TEST_F(FormToolTest, TabMovesThroughTextFieldsAndSkipsReadOnly) {
  open_copy("form-fields.pdf");
  ASSERT_TRUE(wait_widgets());
  click(center_of(0, "name"));
  ASSERT_NE(view()->form_editor(), nullptr);
  key(editor_input(), Qt::Key_Tab);
  ASSERT_TRUE(spin_until([&] { return view()->form_editor() != nullptr; }));
  EXPECT_EQ(view()->form_editor()->widget().ref.name, "address");
  ASSERT_NE(view()->form_editor()->findChild<QPlainTextEdit*>(QStringLiteral("form_text_multi")), nullptr) << "여러 줄";
  key(editor_input(), Qt::Key_Return);  // 여러 줄: Enter 는 줄바꿈, 확정하지 않는다
  EXPECT_NE(view()->form_editor(), nullptr);
  view()->form_editor()->set_text(QStringLiteral("Seoul\nGangnam"));
  key(editor_input(), Qt::Key_Tab);
  ASSERT_TRUE(spin_until([&] { return view()->form_editor() != nullptr && view()->form_editor()->widget().ref.name != "address"; }));
  EXPECT_EQ(view()->form_editor()->widget().ref.name, "zip") << "읽기 전용 empid 를 건너뛴다";
  EXPECT_EQ(qobject_cast<QLineEdit*>(editor_input())->maxLength(), 5);
  view()->form_editor()->set_text(QStringLiteral("06236"));
  key(editor_input(), Qt::Key_Backtab, Qt::ShiftModifier);
  ASSERT_TRUE(spin_until([&] { return view()->form_editor() != nullptr && view()->form_editor()->widget().ref.name == "address"; }));
  view()->cancel_form_editor();
  ASSERT_TRUE(wait_value(0, "address", "Seoul\nGangnam")) << field(0, "address")->value;
  ASSERT_TRUE(wait_value(0, "zip", "06236"));
}

TEST_F(FormToolTest, CheckBoxRadioAndUndoNotice) {
  open_copy("form-fields.pdf");
  ASSERT_TRUE(wait_widgets());
  click(center_of(0, "agree"));
  ASSERT_TRUE(wait_checked(0, "agree", 0, true));
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("폼 선택")));
  settle_clicks();
  click(center_of(0, "gender", 0));
  ASSERT_TRUE(wait_checked(0, "gender", 0, true));
  settle_clicks();
  click(center_of(0, "gender", 1));
  ASSERT_TRUE(wait_checked(0, "gender", 1, true));
  EXPECT_FALSE(field(0, "gender", 0)->checked);
  window_.undo_action()->trigger();
  ASSERT_TRUE(wait_checked(0, "gender", 0, true)) << "이전 선택으로";
  window_.undo_action()->trigger();  // 첫 선택: 해제할 수 없다는 안내, 문서 그대로
  ASSERT_TRUE(spin_until([&] { return idle(); }));
  EXPECT_TRUE(window_.statusBar()->currentMessage().contains(QStringLiteral("라디오")));
  EXPECT_TRUE(field(0, "gender", 0)->checked);
  window_.undo_action()->trigger();  // 스택이 막히지 않는다: 그 앞의 체크박스까지 되돌린다
  ASSERT_TRUE(wait_checked(0, "agree", 0, false));
}

TEST_F(FormToolTest, ChoiceMenuAndDirectEntry) {
  open_copy("form-fields.pdf");
  ASSERT_TRUE(wait_widgets());
  when_popup("form_choice_menu", [](QMenu* menu) {
    for (QAction* a : menu->actions()) {
      if (a->text() == QStringLiteral("Daegu")) a->trigger();
    }
    menu->close();
  });
  click(center_of(0, "city"));
  ASSERT_TRUE(wait_value(0, "city", "Daegu"));
  ASSERT_TRUE(window_.choose_form_option(0, {"subjects", 0}, {0, 3}));
  ASSERT_TRUE(spin_until([&] { const auto w = field(0, "subjects"); return idle() && w && w->selected == std::vector<int>{0, 3}; }));
  ASSERT_TRUE(window_.edit_controller()->undo());
  ASSERT_TRUE(spin_until([&] { const auto w = field(0, "subjects"); return idle() && w && w->selected.empty(); }));
  // 편집 가능 콤보: 메뉴의 "직접 입력…" → 오버레이
  settle_clicks();
  when_popup("form_choice_menu", [](QMenu* menu) {
    for (QAction* a : menu->actions()) {
      if (a->text().startsWith(QStringLiteral("직접 입력"))) a->trigger();
    }
    menu->close();
  });
  click(center_of(0, "city"));
  ASSERT_TRUE(spin_until([&] { return view()->form_editor() != nullptr; }));
  view()->form_editor()->set_text(QStringLiteral("Incheon"));
  key(editor_input(), Qt::Key_Return);
  ASSERT_TRUE(wait_value(0, "city", "Incheon"));
}

TEST_F(FormToolTest, ReadOnlyAndNoPermission) {
  open_copy("form-fields.pdf");
  ASSERT_TRUE(wait_widgets());
  click(center_of(0, "empid"));
  EXPECT_EQ(view()->form_editor(), nullptr);
  EXPECT_TRUE(window_.statusBar()->currentMessage().contains(QStringLiteral("읽기 전용")));
  open_copy("perm-modify-only.pdf");  // 비트 6·9 꺼짐 → 폼 채우기 불가
  EXPECT_FALSE(view()->form_fillable());
}

TEST_F(FormToolTest, SaveCommitsOpenEditor) {
  const auto copy = open_copy("form-fields.pdf");
  ASSERT_TRUE(wait_widgets());
  click(center_of(0, "name"));
  ASSERT_NE(view()->form_editor(), nullptr);
  view()->form_editor()->set_text(QStringLiteral("Saved Name"));
  ASSERT_TRUE(window_.save_to(QString::fromStdString(onpdf::path_to_utf8(copy))));
  ASSERT_TRUE(spin_until([&] { return idle() && !window_.isWindowModified(); }));
  EXPECT_EQ(view()->form_editor(), nullptr);
  window_.edit_controller()->clear();
  ASSERT_TRUE(open_path(copy));  // 다시 열어 값 확인
  ASSERT_TRUE(wait_value(0, "name", "Saved Name"));
}

}  // namespace
}  // namespace onpdf::app
