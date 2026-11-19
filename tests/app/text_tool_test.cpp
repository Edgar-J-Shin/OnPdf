// 텍스트 도구 (#45, offscreen): 도구·커서, 클릭 → 입력 창 → 객체 생성·실행 취소, 빈 입력, 글리프 없음 경고, 권한, 크기, 저장 → 재열기
#include "annotation_dialog.h"
#include "document_view.h"
#include "main_window.h"
#include "onpdf/core/engine/document_handle.h"
#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/platform/path.h"
#include "ui_dispatcher.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QTimer>

#include <filesystem>
#include <set>

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

// 모달 대화상자가 뜨면 처리한다 (objectName 으로 식별)
template <class Fn>
void when_dialog(const char* object_name, Fn fn) {
  auto* poll = new QTimer();
  poll->setInterval(10);
  QObject::connect(poll, &QTimer::timeout, poll, [poll, object_name, fn] {
    QWidget* w = QApplication::activeModalWidget();
    if (w == nullptr || w->objectName() != QLatin1String(object_name)) return;
    fn(w);
    poll->deleteLater();
  });
  poll->start();
}

class TextToolTest : public ::testing::Test {
 protected:
  TextToolTest() : worker_(options()), window_(worker_, dispatcher_) {
    window_.resize(900, 900);
    window_.show();
    // 번들 폰트 대신 저장소 서브셋 폰트 (CI 에는 번들이 없다)
    EXPECT_TRUE(window_.set_text_font(QString::fromStdString(onpdf::path_to_utf8(testdata("fonts") / "NotoSansKR-subset.ttf"))));
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
    std::filesystem::copy_file(testdata(name), copy);
    EXPECT_TRUE(open_path(copy));
    return copy;
  }
  bool idle() { return !window_.edit_controller()->busy() && !window_.saving(); }
  bool wait_idle() { return spin_until([&] { return idle(); }); }
  // 텍스트 상자 캐시가 n 개 상자를 알 때까지
  bool wait_text_boxes(int page, std::size_t n) {
    return spin_until([&] {
      if (!idle() || view()->text_objects(page) == nullptr) return false;
      std::set<std::string> boxes;
      for (const auto& t : *view()->text_objects(page)) boxes.insert(t.box);
      return boxes.size() == n;
    });
  }
  void drag(QPointF from, QPointF to) {
    send_mouse(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    send_mouse(QEvent::MouseMove, (from + to) / 2, Qt::NoButton, Qt::LeftButton);
    send_mouse(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
    send_mouse(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
  }
  // 연속 클릭 판정(더블 클릭 → 편집 창)을 피하려고 다음 누름 전에 잠시 기다린다
  void settle_clicks() {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 600) QApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  void double_click(QPointF at) {
    click(at);
    QMouseEvent dbl(QEvent::MouseButtonDblClick, at, view()->viewport()->mapToGlobal(at.toPoint()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view()->viewport(), &dbl);
    send_mouse(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
  }
  bool wait_text_contains(int page, const QString& needle) {
    return spin_until([&] {
      if (!idle() || view()->text_index(page) == nullptr) return false;
      return QString::fromStdString(view()->text_index(page)->all_text()).contains(needle);
    });
  }
  bool wait_text_missing(int page, const QString& needle) {
    return spin_until([&] {
      if (!idle() || view()->text_index(page) == nullptr) return false;
      return !QString::fromStdString(view()->text_index(page)->all_text()).contains(needle);
    });
  }
  void send_mouse(QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent ev(type, pos, view()->viewport()->mapToGlobal(pos.toPoint()), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(view()->viewport(), &ev);
  }
  void click(QPointF at) {
    send_mouse(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    send_mouse(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
  }
  void use_tool(AnnotationTool tool) { window_.tool_action(tool)->trigger(); }

  ScopedTempDir dir_;
  UiDispatcher dispatcher_;
  engine::PdfiumWorker worker_;
  MainWindow window_;
};

TEST_F(TextToolTest, ClickOpensDialogAndAddsText) {
  open_copy("simple.pdf");
  ASSERT_TRUE(spin_until([&] { return view()->text_index(0) != nullptr; }));
  EXPECT_TRUE(window_.tool_action(AnnotationTool::kText)->isEnabled());
  EXPECT_TRUE(window_.text_size_combo()->isEnabled());
  use_tool(AnnotationTool::kText);
  EXPECT_EQ(view()->tool(), AnnotationTool::kText);
  send_mouse(QEvent::MouseMove, view()->page_point_to_viewport(0, {100, 400}), Qt::NoButton, Qt::NoButton);
  EXPECT_EQ(view()->viewport()->cursor().shape(), Qt::IBeamCursor);
  when_dialog("text_input", [](QWidget* w) {
    auto* edit = w->findChild<QPlainTextEdit*>(QStringLiteral("note_text"));
    ASSERT_NE(edit, nullptr);
    edit->setPlainText(QStringLiteral("한글 텍스트 추가\n둘째 줄"));
    static_cast<AnnotationDialog*>(w)->accept();
  });
  click(view()->page_point_to_viewport(0, {100, 400}));
  ASSERT_TRUE(wait_text_contains(0, QStringLiteral("한글 텍스트 추가")));
  EXPECT_TRUE(QString::fromStdString(view()->text_index(0)->all_text()).contains(QStringLiteral("둘째 줄")));
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("텍스트 추가")));
  EXPECT_TRUE(window_.isWindowModified());
  // 추가한 글자가 클릭 자리에 (쪽 포인트)
  bool found = false;
  for (const auto& ch : view()->text_index(0)->chars) {
    if (ch.code == U'한') {
      found = true;
      EXPECT_NEAR(ch.box.x, 100.0, 3.0);
      EXPECT_NEAR(ch.box.y, 400.0, 10.0);
    }
  }
  EXPECT_TRUE(found);
  ASSERT_TRUE(window_.edit_controller()->undo());
  ASSERT_TRUE(wait_text_missing(0, QStringLiteral("한글 텍스트 추가")));
  ASSERT_TRUE(window_.edit_controller()->redo());
  ASSERT_TRUE(wait_text_contains(0, QStringLiteral("한글 텍스트 추가")));
}

TEST_F(TextToolTest, EmptyInputAndCancelMakeNothing) {
  open_copy("simple.pdf");
  ASSERT_TRUE(wait_idle());
  use_tool(AnnotationTool::kText);
  when_dialog("text_input", [](QWidget* w) {
    w->findChild<QPlainTextEdit*>(QStringLiteral("note_text"))->setPlainText(QStringLiteral("   \n"));
    static_cast<AnnotationDialog*>(w)->accept();
  });
  click(view()->page_point_to_viewport(0, {100, 400}));
  ASSERT_TRUE(wait_idle());
  EXPECT_FALSE(window_.isWindowModified());
  when_dialog("text_input", [](QWidget* w) { static_cast<AnnotationDialog*>(w)->reject(); });
  click(view()->page_point_to_viewport(0, {100, 450}));
  ASSERT_TRUE(wait_idle());
  EXPECT_FALSE(window_.isWindowModified());
  EXPECT_FALSE(window_.add_text(0, {100, 100}, QStringLiteral("  ")));
}

TEST_F(TextToolTest, MissingGlyphsAreRefusedWithWarning) {
  open_copy("simple.pdf");
  ASSERT_TRUE(wait_idle());
  use_tool(AnnotationTool::kText);
  bool warned = false;
  when_dialog("text_input", [](QWidget* w) {
    w->findChild<QPlainTextEdit*>(QStringLiteral("note_text"))->setPlainText(QStringLiteral("이모지 😀"));
    static_cast<AnnotationDialog*>(w)->accept();
  });
  when_dialog("text_missing_glyphs", [&](QWidget* w) {
    warned = true;
    auto* box = static_cast<QMessageBox*>(w);
    EXPECT_TRUE(box->text().contains(QStringLiteral("😀")));
    box->button(QMessageBox::Ok)->click();
    // 경고 뒤 다시 뜨는 입력 창은 취소
    when_dialog("text_input", [](QWidget* again) { static_cast<AnnotationDialog*>(again)->reject(); });
  });
  click(view()->page_point_to_viewport(0, {100, 400}));
  ASSERT_TRUE(spin_until([&] { return warned && idle() && QApplication::activeModalWidget() == nullptr; }));
  EXPECT_FALSE(window_.isWindowModified());
}

TEST_F(TextToolTest, SizeComboAppliesToNewText) {
  open_copy("simple.pdf");
  ASSERT_TRUE(spin_until([&] { return view()->text_index(0) != nullptr; }));
  window_.set_text_size(48);
  EXPECT_EQ(window_.text_size_combo()->currentData().toInt(), 48);
  ASSERT_TRUE(window_.add_text(0, {72, 300}, QStringLiteral("입력 저장")));
  ASSERT_TRUE(wait_text_contains(0, QStringLiteral("입력 저장")));
  double height = 0;
  for (const auto& ch : view()->text_index(0)->chars) {
    if (ch.code == U'입') height = ch.box.h;
  }
  // 글꼴에 없는 글자는 Command 가 거절한다 (대화상자 없는 경로)
  EXPECT_TRUE(window_.add_text(0, {72, 400}, QStringLiteral("큰 😀")));  // 시작은 되지만
  ASSERT_TRUE(wait_idle());
  EXPECT_FALSE(QString::fromStdString(view()->text_index(0)->all_text()).contains(QStringLiteral("큰")));
  EXPECT_GT(height, 30.0) << "48pt 글자 상자";
}

TEST_F(TextToolTest, PermissionAndFontGateTheTool) {
  ASSERT_TRUE(open_path(testdata("encrypted-owner-only.pdf")));  // 내용 수정 금지(비트 4 없음)
  ASSERT_TRUE(wait_idle());
  EXPECT_FALSE(window_.tool_action(AnnotationTool::kText)->isEnabled());
  EXPECT_FALSE(window_.text_size_combo()->isEnabled());
  EXPECT_FALSE(window_.add_text(0, {100, 100}, QStringLiteral("x")));
  open_copy("simple.pdf");
  ASSERT_TRUE(wait_idle());
  EXPECT_TRUE(window_.tool_action(AnnotationTool::kText)->isEnabled());
  use_tool(AnnotationTool::kText);
  // 글꼴이 없으면 도구가 꺼지고 선택 도구로
  EXPECT_FALSE(window_.set_text_font(QStringLiteral("/nonexistent/font.ttf")));
  EXPECT_FALSE(window_.has_text_font());
  EXPECT_FALSE(window_.tool_action(AnnotationTool::kText)->isEnabled());
  EXPECT_EQ(view()->tool(), AnnotationTool::kSelect);
  EXPECT_TRUE(window_.tool_action(AnnotationTool::kText)->statusTip().contains(QStringLiteral("글꼴")));
}

// 내용 수정(비트 4)만 있는 문서: 텍스트 도구는 켜지고 주석 도구는 꺼진다. 요청 게이트도 비트 4 (리뷰 W2)
TEST_F(TextToolTest, ModifyOnlyPermissionEnablesTextButNotAnnotations) {
  const auto copy = open_copy("perm-modify-only.pdf");
  ASSERT_TRUE(spin_until([&] { return view()->text_index(0) != nullptr; }));
  EXPECT_TRUE(window_.tool_action(AnnotationTool::kText)->isEnabled());
  EXPECT_FALSE(window_.tool_action(AnnotationTool::kHighlight)->isEnabled());
  EXPECT_FALSE(window_.add_note(0, {100, 100}, QStringLiteral("x")));
  // NFD 입력은 NFC 로 정규화돼 들어간다 (리뷰 W3)
  QString failure;
  QObject::connect(window_.edit_controller(), &EditController::failed, &window_, [&](onpdf::ErrorCode, const QString& m) { failure = m; });
  ASSERT_TRUE(window_.add_text(0, {72, 300}, QString::fromUtf8("\xE1\x84\x92\xE1\x85\xA1\xE1\x86\xAB \xE1\x84\x80\xE1\x85\xB3\xE1\x86\xAF")));  // 한글 (NFD)
  const QString nfd = QString::fromUtf8("\xE1\x84\x92\xE1\x85\xA1\xE1\x86\xAB");
  EXPECT_EQ(nfd.normalized(QString::NormalizationForm_C), QStringLiteral("한")) << "Qt NFC 가 한글 자모를 합성한다";
  ASSERT_TRUE(wait_idle());
  EXPECT_TRUE(window_.edit_controller()->can_undo()) << "명령이 스택에 올라갔다";
  ASSERT_TRUE(wait_text_contains(0, QStringLiteral("한 글"))) << "failed: " << failure.toStdString();  // 입력은 "한 글" (NFD, 공백 포함)
}

TEST_F(TextToolTest, SavedTextReopens) {
  const auto copy = open_copy("simple.pdf");
  ASSERT_TRUE(spin_until([&] { return view()->text_index(0) != nullptr; }));
  ASSERT_TRUE(window_.add_text(0, {72, 300}, QStringLiteral("저장 열기 검증")));
  ASSERT_TRUE(wait_text_contains(0, QStringLiteral("저장 열기 검증")));
  window_.save_action()->trigger();
  ASSERT_TRUE(spin_until([&] { return !window_.saving() && !window_.isWindowModified(); }));
  const auto again = dir_ / "simple-again.pdf";
  std::filesystem::copy_file(copy, again);
  ASSERT_TRUE(open_path(again));
  ASSERT_TRUE(wait_text_contains(0, QStringLiteral("저장 열기 검증")));
}

// ---------------------------------------------------------------- 텍스트 상자 편집 (#47)

TEST_F(TextToolTest, ClickSelectsBoxAndDragMoves) {
  open_copy("simple.pdf");
  ASSERT_TRUE(spin_until([&] { return view()->text_index(0) != nullptr; }));
  ASSERT_TRUE(window_.add_text(0, {100, 300}, QStringLiteral("한글 텍스트\n둘째 줄")));
  ASSERT_TRUE(wait_text_boxes(0, 1));
  const auto& first = (*view()->text_objects(0))[0];
  const QPointF inside = view()->page_point_to_viewport(0, {first.bbox.x + 5, first.bbox.y + first.bbox.h / 2});
  click(inside);
  ASSERT_TRUE(view()->selected_text_box().has_value());
  EXPECT_EQ(view()->selected_text_box()->lines, 2);
  EXPECT_EQ(view()->selected_text_box()->text, "한글 텍스트\n둘째 줄");
  EXPECT_TRUE(window_.statusBar()->currentMessage().contains(QStringLiteral("2줄")));
  EXPECT_TRUE(window_.delete_annotation_action()->isEnabled());
  send_mouse(QEvent::MouseMove, inside, Qt::NoButton, Qt::NoButton);
  EXPECT_EQ(view()->viewport()->cursor().shape(), Qt::SizeAllCursor);
  // 3pt 드래그는 이동이 아니다 (연속 클릭으로 보이지 않게 잠시 기다린다)
  settle_clicks();
  drag(inside, inside + QPointF(2, 2));
  ASSERT_TRUE(wait_idle());
  EXPECT_FALSE(window_.edit_controller()->can_undo() && window_.undo_action()->text().contains(QStringLiteral("객체 이동")));
  // 이동
  const double before_x = first.bbox.x;
  const QPointF to = view()->page_point_to_viewport(0, {first.bbox.x + 5 + 50, first.bbox.y + first.bbox.h / 2 + 30});
  settle_clicks();
  drag(inside, to);
  ASSERT_TRUE(spin_until([&] { return idle() && view()->text_objects(0) && std::abs((*view()->text_objects(0))[0].bbox.x - (before_x + 50)) < 2.0; }));
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("객체 이동")));
  ASSERT_TRUE(spin_until([&] { return view()->selected_text_box().has_value(); })) << "편집 뒤 같은 상자를 다시 고른다";
  EXPECT_NEAR(view()->selected_text_box()->bbox.x, before_x + 50, 2.0);
  ASSERT_TRUE(window_.edit_controller()->undo());
  ASSERT_TRUE(spin_until([&] { return idle() && view()->text_objects(0) && std::abs((*view()->text_objects(0))[0].bbox.x - before_x) < 2.0; }));
  ASSERT_TRUE(spin_until([&] { return view()->selected_text_box().has_value(); })) << "이동 취소 뒤에도 재선택";
  EXPECT_NEAR(view()->selected_text_box()->bbox.x, before_x, 2.0);
  // Esc 해제
  QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QApplication::sendEvent(view(), &esc);
  EXPECT_FALSE(view()->selected_text_box().has_value());
}

TEST_F(TextToolTest, DeleteKeyRemovesBoxAndUndoRestores) {
  open_copy("simple.pdf");
  ASSERT_TRUE(spin_until([&] { return view()->text_index(0) != nullptr; }));
  ASSERT_TRUE(window_.add_text(0, {100, 300}, QStringLiteral("둘째 줄 텍스트")));
  ASSERT_TRUE(wait_text_boxes(0, 1));
  const auto box = (*view()->text_objects(0))[0];
  click(view()->page_point_to_viewport(0, {box.bbox.x + 5, box.bbox.y + box.bbox.h / 2}));
  ASSERT_TRUE(view()->selected_text_box().has_value());
  QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
  QApplication::sendEvent(view(), &del);
  ASSERT_TRUE(wait_text_boxes(0, 0));
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("삭제")));
  EXPECT_FALSE(view()->selected_text_box().has_value());
  ASSERT_TRUE(wait_text_missing(0, QStringLiteral("둘째 줄 텍스트")));
  ASSERT_TRUE(window_.edit_controller()->undo());
  ASSERT_TRUE(wait_text_boxes(0, 1));
  EXPECT_EQ((*view()->text_objects(0))[0].id, box.id) << "같은 id 로 복원";
  ASSERT_TRUE(wait_text_contains(0, QStringLiteral("둘째 줄 텍스트")));
}

TEST_F(TextToolTest, DoubleClickEditsAndPropertiesApplyToSelection) {
  open_copy("simple.pdf");
  ASSERT_TRUE(spin_until([&] { return view()->text_index(0) != nullptr; }));
  ASSERT_TRUE(window_.add_text(0, {100, 300}, QStringLiteral("한글 텍스트")));
  ASSERT_TRUE(wait_text_boxes(0, 1));
  const auto box = (*view()->text_objects(0))[0];
  const QPointF inside = view()->page_point_to_viewport(0, {box.bbox.x + 5, box.bbox.y + box.bbox.h / 2});
  click(inside);
  ASSERT_TRUE(view()->selected_text_box().has_value());
  // 두 번 클릭 → 원문이 채워진 입력 창 → 수정
  QString prefilled;
  when_dialog("text_input", [&](QWidget* w) {
    auto* edit = w->findChild<QPlainTextEdit*>(QStringLiteral("note_text"));
    prefilled = edit->toPlainText();
    edit->setPlainText(QStringLiteral("저장 열기\n둘째 줄"));
    static_cast<AnnotationDialog*>(w)->accept();
  });
  double_click(inside);
  ASSERT_TRUE(wait_text_contains(0, QStringLiteral("저장 열기")));
  EXPECT_EQ(prefilled, QStringLiteral("한글 텍스트"));
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("텍스트 수정")));
  ASSERT_TRUE(spin_until([&] { return view()->selected_text_box().has_value() && view()->selected_text_box()->lines == 2; }));
  EXPECT_EQ((*view()->text_objects(0))[0].id, box.id) << "첫 줄 id 재사용";
  // 크기 콤보 → 선택한 상자만 (기본값은 그대로)
  const double default_size = window_.text_size();
  window_.set_text_size(36);
  ASSERT_TRUE(spin_until([&] { return idle() && view()->text_objects(0) && std::abs((*view()->text_objects(0))[0].font_size - 36.0) < 0.01; }));
  EXPECT_DOUBLE_EQ(window_.text_size(), default_size);
  ASSERT_TRUE(spin_until([&] { return view()->selected_text_box().has_value(); }));
  EXPECT_EQ(window_.text_size_combo()->currentData().toInt(), 36) << "콤보는 선택한 상자의 크기를 비춘다";
  // 색 → 선택한 상자만
  window_.set_annotation_color({0, 0, 255, 255});
  ASSERT_TRUE(spin_until([&] { return idle() && view()->text_objects(0) && (*view()->text_objects(0))[0].color.b == 255 && (*view()->text_objects(0))[0].color.r == 0; }));
  EXPECT_NE(window_.annotation_color().b, 255) << "기본 색은 그대로";
  ASSERT_TRUE(window_.edit_controller()->undo());
  ASSERT_TRUE(spin_until([&] { return idle() && view()->text_objects(0) && (*view()->text_objects(0))[0].color.r == 0 && (*view()->text_objects(0))[0].color.b == 0; }));
}

TEST_F(TextToolTest, AnnotationWinsHitTestAndPermissionGatesEditing) {
  open_copy("simple.pdf");
  ASSERT_TRUE(spin_until([&] { return view()->text_index(0) != nullptr; }));
  ASSERT_TRUE(window_.add_text(0, {100, 300}, QStringLiteral("한글 텍스트")));
  ASSERT_TRUE(wait_text_boxes(0, 1));
  const auto box = (*view()->text_objects(0))[0];
  // 같은 자리에 메모 → 클릭하면 주석이 우선
  ASSERT_TRUE(window_.add_note(0, {box.bbox.x + 10, box.bbox.y + 10}, QStringLiteral("메모")));
  ASSERT_TRUE(spin_until([&] { return idle() && view()->annotations(0) && view()->annotations(0)->size() == 1; }));
  click(view()->page_point_to_viewport(0, {box.bbox.x + 10, box.bbox.y + 10}));
  EXPECT_TRUE(view()->selected_annotation().has_value());
  EXPECT_FALSE(view()->selected_text_box().has_value());
  // 선택이 주석일 때 텍스트 상자 삭제는 아무것도 하지 않는다 (권한 게이트는 PermissionAndFontGateTheTool 이 덮는다)
  EXPECT_FALSE(window_.delete_selected_text_box());
  // 상자를 고른 뒤 주석을 고르면 상자 선택은 해제된다 (리뷰 W3)
  QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QApplication::sendEvent(view(), &esc);
  settle_clicks();
  click(view()->page_point_to_viewport(0, {box.bbox.right() - 5, box.bbox.y + box.bbox.h / 2}));
  ASSERT_TRUE(view()->selected_text_box().has_value());
  settle_clicks();
  click(view()->page_point_to_viewport(0, {box.bbox.x + 10, box.bbox.y + 10}));
  EXPECT_TRUE(view()->selected_annotation().has_value());
  EXPECT_FALSE(view()->selected_text_box().has_value());
  // 도구를 바꾸면 상자 선택이 해제된다
  QApplication::sendEvent(view(), &esc);
  settle_clicks();
  click(view()->page_point_to_viewport(0, {box.bbox.right() - 5, box.bbox.y + box.bbox.h / 2}));
  ASSERT_TRUE(view()->selected_text_box().has_value());
  use_tool(AnnotationTool::kHighlight);
  EXPECT_FALSE(view()->selected_text_box().has_value());
  use_tool(AnnotationTool::kSelect);
}

TEST_F(TextToolTest, SavedBoxReopensEditable) {
  const auto copy = open_copy("simple.pdf");
  ASSERT_TRUE(spin_until([&] { return view()->text_index(0) != nullptr; }));
  ASSERT_TRUE(window_.add_text(0, {100, 300}, QStringLiteral("저장 열기\n둘째 줄")));
  ASSERT_TRUE(wait_text_boxes(0, 1));
  window_.save_action()->trigger();
  ASSERT_TRUE(spin_until([&] { return !window_.saving() && !window_.isWindowModified(); }));
  const auto again = dir_ / "simple-again.pdf";
  std::filesystem::copy_file(copy, again);
  ASSERT_TRUE(open_path(again));
  ASSERT_TRUE(wait_text_boxes(0, 1));
  const auto box = (*view()->text_objects(0))[0];
  EXPECT_EQ(box.text, "저장 열기");
  click(view()->page_point_to_viewport(0, {box.bbox.x + 5, box.bbox.y + box.bbox.h / 2}));
  ASSERT_TRUE(view()->selected_text_box().has_value());
  EXPECT_EQ(view()->selected_text_box()->lines, 2);
  QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
  QApplication::sendEvent(view(), &del);
  ASSERT_TRUE(wait_text_boxes(0, 0));
  ASSERT_TRUE(window_.edit_controller()->undo());
  ASSERT_TRUE(wait_text_boxes(0, 1));
}

}  // namespace
}  // namespace onpdf::app
