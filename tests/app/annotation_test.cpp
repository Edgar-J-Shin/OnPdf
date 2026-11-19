// 주석 UI (#35): 도구 모드, 드래그 → 마크업, 메모 대화상자, 펜, 선택·삭제·색 변경, 권한, 서명 경고, 인쇄 포함
#include "annotation_dialog.h"
#include "document_view.h"
#include "main_window.h"
#include "onpdf/core/engine/document_handle.h"
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
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPrinter>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>

namespace onpdf::app {
namespace {

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

// 이름이 object_name 인 모달 대화상자가 뜨면 처리한다
template <class Fn>
void when_dialog(const char* object_name, Fn fn) {
  auto* poll = new QTimer();
  poll->setInterval(10);
  QObject::connect(poll, &QTimer::timeout, poll, [poll, object_name, fn] {
    QWidget* w = QApplication::activeModalWidget();
    if (w == nullptr || w->objectName() != QString::fromLatin1(object_name)) return;
    fn(w);
    poll->deleteLater();
  });
  poll->start();
}

class AnnotationTest : public ::testing::Test {
 protected:
  AnnotationTest() : worker_(options()), window_(worker_, dispatcher_) {
    window_.resize(900, 900);
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
  bool open(const char* name) { return open_path(testdata(name)); }
  std::filesystem::path open_copy(const char* name) {
    const auto copy = dir_ / name;
    std::filesystem::copy_file(testdata(name), copy);
    EXPECT_TRUE(open_path(copy));
    return copy;
  }
  bool idle() { return !window_.edit_controller()->busy() && !window_.saving(); }
  bool wait_idle() { return spin_until([&] { return idle(); }); }
  bool wait_text(int page) { return spin_until([&] { return view()->text_index(page) != nullptr; }); }
  // 쪽의 주석 목록이 n 개로 갱신될 때까지
  bool wait_annotations(int page, std::size_t n) {
    return spin_until([&] { return idle() && view()->annotations(page) != nullptr && view()->annotations(page)->size() == n; });
  }
  void send_mouse(QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent ev(type, pos, view()->viewport()->mapToGlobal(pos.toPoint()), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(view()->viewport(), &ev);
  }
  void drag(QPointF from, QPointF to) {
    send_mouse(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    send_mouse(QEvent::MouseMove, (from + to) / 2, Qt::NoButton, Qt::LeftButton);
    send_mouse(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
    send_mouse(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
  }
  void click(QPointF at) {
    send_mouse(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    send_mouse(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
  }
  QPointF char_center(int page, int ch) {
    const auto& box = view()->text_index(page)->chars[static_cast<std::size_t>(ch)].box;
    return view()->page_point_to_viewport(page, {box.x + box.w / 2, box.y + box.h / 2});
  }
  // 첫 쪽 글자 0~9 를 드래그
  void drag_first_word(int page = 0) { drag(char_center(page, 0), char_center(page, 9)); }
  void use_tool(AnnotationTool tool) { window_.tool_action(tool)->trigger(); }

  ScopedTempDir dir_;
  UiDispatcher dispatcher_;
  engine::PdfiumWorker worker_;
  MainWindow window_;
};

TEST_F(AnnotationTest, ToolSwitchChangesCursor) {
  ASSERT_TRUE(open("simple.pdf"));
  EXPECT_EQ(view()->tool(), AnnotationTool::kSelect);
  EXPECT_TRUE(window_.tool_action(AnnotationTool::kSelect)->isChecked());
  use_tool(AnnotationTool::kNote);
  EXPECT_EQ(view()->tool(), AnnotationTool::kNote);
  send_mouse(QEvent::MouseMove, QPointF(300, 300), Qt::NoButton, Qt::NoButton);
  EXPECT_EQ(view()->viewport()->cursor().shape(), Qt::CrossCursor);
  use_tool(AnnotationTool::kSelect);
  send_mouse(QEvent::MouseMove, QPointF(300, 300), Qt::NoButton, Qt::NoButton);
  EXPECT_NE(view()->viewport()->cursor().shape(), Qt::CrossCursor);
}

TEST_F(AnnotationTest, HighlightDragCreatesAnnotationAndUndoRemovesIt) {
  open_copy("simple.pdf");
  ASSERT_TRUE(wait_text(0));
  ASSERT_TRUE(wait_annotations(0, 0));
  use_tool(AnnotationTool::kHighlight);
  drag_first_word();
  ASSERT_TRUE(wait_annotations(0, 1));
  const auto& a = (*view()->annotations(0))[0];
  EXPECT_EQ(a.kind, engine::AnnotationKind::kHighlight);
  EXPECT_EQ(a.flags & engine::kAnnotFlagPrint, engine::kAnnotFlagPrint);
  EXPECT_FALSE(view()->has_selection()) << "주석이 선택 역할을 이어받는다";
  EXPECT_EQ(view()->tool(), AnnotationTool::kHighlight) << "명령을 실행해도 도구는 유지된다 (리뷰 W1)";
  EXPECT_TRUE(window_.isWindowModified());
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("하이라이트 추가")));
  ASSERT_TRUE(window_.edit_controller()->undo());
  ASSERT_TRUE(wait_annotations(0, 0));
  EXPECT_FALSE(window_.isWindowModified());
  EXPECT_EQ(view()->tool(), AnnotationTool::kHighlight);
  ASSERT_TRUE(window_.edit_controller()->redo());
  ASSERT_TRUE(wait_annotations(0, 1));
  // 바쁜 동안의 요청은 거절되지만 도구는 남는다
  window_.rotate_current_page(1);
  ASSERT_TRUE(window_.edit_controller()->busy());
  EXPECT_FALSE(window_.add_note(0, {100, 100}, QStringLiteral("x")));
  EXPECT_EQ(view()->tool(), AnnotationTool::kHighlight);
  ASSERT_TRUE(wait_idle());
}

TEST_F(AnnotationTest, MarkupShortcutUsesCurrentSelection) {
  open_copy("simple.pdf");
  ASSERT_TRUE(wait_text(0));
  drag_first_word();  // 선택 도구: 텍스트 선택
  ASSERT_TRUE(view()->has_selection());
  ASSERT_TRUE(window_.add_markup_from_selection(engine::AnnotationKind::kUnderline));
  ASSERT_TRUE(wait_annotations(0, 1));
  EXPECT_EQ((*view()->annotations(0))[0].kind, engine::AnnotationKind::kUnderline);
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("밑줄 추가")));
  EXPECT_FALSE(window_.add_markup_from_selection(engine::AnnotationKind::kStrikeOut)) << "선택이 지워졌다";
}

TEST_F(AnnotationTest, NoteToolOpensEditorAndCreatesNote) {
  open_copy("simple.pdf");
  ASSERT_TRUE(wait_annotations(0, 0));
  use_tool(AnnotationTool::kNote);
  bool asked = false;
  when_dialog("note_editor", [&](QWidget* w) {
    asked = true;
    w->findChild<QPlainTextEdit*>(QStringLiteral("note_text"))->setPlainText(QStringLiteral("검토 필요"));
    qobject_cast<QDialog*>(w)->accept();
  });
  click(view()->page_point_to_viewport(0, {300, 300}));
  ASSERT_TRUE(spin_until([&] { return asked; }));
  ASSERT_TRUE(wait_annotations(0, 1));
  const auto& a = (*view()->annotations(0))[0];
  EXPECT_EQ(a.kind, engine::AnnotationKind::kText);
  EXPECT_EQ(a.contents, "검토 필요");
  EXPECT_NEAR(a.rect.x + a.rect.w / 2, 300, 0.1);
  EXPECT_NEAR(a.rect.y + a.rect.h / 2, 300, 0.1);
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("메모 추가")));
  // 빈 내용으로 확인하면 만들지 않는다
  when_dialog("note_editor", [&](QWidget* w) { qobject_cast<QDialog*>(w)->accept(); });
  click(view()->page_point_to_viewport(0, {200, 500}));
  ASSERT_TRUE(wait_idle());
  EXPECT_EQ(view()->annotations(0)->size(), 1u);
}

// 펜이 쪽 아래로 나가도 점이 쪽 맨 위로 튀지 않는다 (리뷰 W2): 시작 쪽 좌표로 투영한 뒤 클램프
TEST_F(AnnotationTest, PenDragBeyondPageEdgeStaysAtEdge) {
  open_copy("simple.pdf");
  ASSERT_TRUE(wait_annotations(0, 0));
  view()->set_zoom(0.5);  // 1쪽 아래 2쪽이 보이게
  use_tool(AnnotationTool::kPen);
  const QPointF start = view()->page_point_to_viewport(0, {300, 700});
  const QPointF beyond = view()->page_point_to_viewport(0, {300, 900});  // 1쪽(792pt) 아래 = 2쪽 위
  send_mouse(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
  send_mouse(QEvent::MouseMove, view()->page_point_to_viewport(0, {300, 780}), Qt::NoButton, Qt::LeftButton);
  send_mouse(QEvent::MouseMove, beyond, Qt::NoButton, Qt::LeftButton);
  send_mouse(QEvent::MouseButtonRelease, beyond, Qt::LeftButton, Qt::NoButton);
  ASSERT_TRUE(wait_annotations(0, 1));
  const auto& a = (*view()->annotations(0))[0];
  EXPECT_EQ(a.kind, engine::AnnotationKind::kInk);
  EXPECT_GT(a.rect.y, 600) << "쪽 맨 위(y≈0)로 튄 점이 없어야 한다";
  EXPECT_GE(a.rect.bottom(), 790) << "쪽 아래 가장자리에서 멈춘다";
  EXPECT_EQ(view()->annotations(1) == nullptr ? 0u : view()->annotations(1)->size(), 0u);
}

// 화면에 없는(NoView) 주석은 고를 수 없다 (리뷰 W4)
TEST_F(AnnotationTest, HiddenAnnotationsAreNotSelectable) {
  ASSERT_TRUE(open("print-flags.pdf"));
  ASSERT_TRUE(wait_annotations(0, 2));
  const engine::AnnotationInfo* hidden = nullptr;
  const engine::AnnotationInfo* visible = nullptr;
  for (const auto& a : *view()->annotations(0)) {
    if (a.flags & engine::kAnnotFlagNoView) hidden = &a; else visible = &a;
  }
  ASSERT_NE(hidden, nullptr);
  ASSERT_NE(visible, nullptr);
  const auto center = [](const onpdf::Rect& r) { return onpdf::Point{r.x + r.w / 2, r.y + r.h / 2}; };
  click(view()->page_point_to_viewport(0, center(hidden->rect)));
  EXPECT_FALSE(view()->selected_annotation().has_value());
  click(view()->page_point_to_viewport(0, center(visible->rect)));
  ASSERT_TRUE(view()->selected_annotation().has_value());
  EXPECT_EQ(view()->selected_annotation()->kind, engine::AnnotationKind::kSquare);
}

TEST_F(AnnotationTest, PenDragCreatesInk) {
  open_copy("simple.pdf");
  ASSERT_TRUE(wait_annotations(0, 0));
  use_tool(AnnotationTool::kPen);
  drag(view()->page_point_to_viewport(0, {100, 400}), view()->page_point_to_viewport(0, {200, 420}));
  ASSERT_TRUE(wait_annotations(0, 1));
  const auto& a = (*view()->annotations(0))[0];
  EXPECT_EQ(a.kind, engine::AnnotationKind::kInk);
  EXPECT_LE(a.rect.x, 100);
  EXPECT_GE(a.rect.right(), 200);
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("펜 추가")));
}

TEST_F(AnnotationTest, SelectDeleteAndRecolorAnnotation) {
  open_copy("simple.pdf");
  ASSERT_TRUE(wait_annotations(0, 0));
  ASSERT_TRUE(window_.add_note(0, {300, 300}, QStringLiteral("메모")));
  ASSERT_TRUE(wait_annotations(0, 1));
  use_tool(AnnotationTool::kSelect);
  click(view()->page_point_to_viewport(0, {300, 300}));
  ASSERT_TRUE(view()->selected_annotation().has_value());
  EXPECT_EQ(view()->selected_annotation()->kind, engine::AnnotationKind::kText);
  EXPECT_TRUE(window_.delete_annotation_action()->isEnabled());
  EXPECT_TRUE(window_.statusBar()->currentMessage().contains(QStringLiteral("메모")));
  // 색 변경 → 선택한 주석만 바뀌고 기본 색은 그대로 (리뷰 I3)
  const engine::Color default_before = window_.annotation_color();
  window_.set_annotation_color({0, 200, 0, 255});
  ASSERT_TRUE(wait_idle());
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("주석 색 변경")));
  EXPECT_EQ(window_.annotation_color(), default_before);
  ASSERT_TRUE(wait_annotations(0, 1));
  // (색 값 자체는 렌더로 AP 가 생기면 조회 API 가 읽지 못한다 — 색 왕복은 단위 테스트가 보장한다)
  // 선택이 없으면 기본 색이 바뀐다
  QKeyEvent esc0(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QApplication::sendEvent(view(), &esc0);
  window_.set_annotation_color({0, 150, 255, 255});
  EXPECT_EQ(window_.annotation_color(), (engine::Color{0, 150, 255, 255}));
  EXPECT_FALSE(window_.edit_controller()->busy());
  // 다시 선택 후 Delete
  click(view()->page_point_to_viewport(0, {300, 300}));
  ASSERT_TRUE(view()->selected_annotation().has_value());
  QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
  QApplication::sendEvent(view(), &del);
  ASSERT_TRUE(wait_annotations(0, 0));
  EXPECT_FALSE(view()->selected_annotation().has_value());
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("주석 삭제")));
  ASSERT_TRUE(window_.edit_controller()->undo());
  ASSERT_TRUE(wait_annotations(0, 1));
  EXPECT_EQ((*view()->annotations(0))[0].contents, "메모");
  // 다시 고른 뒤 Escape 로 해제
  click(view()->page_point_to_viewport(0, {300, 300}));
  ASSERT_TRUE(view()->selected_annotation().has_value());
  QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QApplication::sendEvent(view(), &esc);
  EXPECT_FALSE(view()->selected_annotation().has_value());
}

TEST_F(AnnotationTest, ExistingLinksAreNotSelectable) {
  ASSERT_TRUE(open("annotations.pdf"));
  ASSERT_TRUE(wait_annotations(0, 6));
  // 세 번째 링크 사각형(사용자 좌표 72..300 × 320..340 → 쪽 포인트 y = 792-340..792-320) 가운데 클릭
  click(view()->page_point_to_viewport(0, {186, 462}));
  EXPECT_FALSE(view()->selected_annotation().has_value());
  // Square(사용자 72..300 × 480..620 → 쪽 포인트 y 172..312) 는 선택된다
  click(view()->page_point_to_viewport(0, {186, 242}));
  ASSERT_TRUE(view()->selected_annotation().has_value());
  EXPECT_EQ(view()->selected_annotation()->kind, engine::AnnotationKind::kSquare);
}

// ---------------------------------------------------------------- 도형 (#39)

TEST_F(AnnotationTest, SquareDragCreatesShapeAndTinyDragDoesNot) {
  open_copy("simple.pdf");
  ASSERT_TRUE(wait_annotations(0, 0));
  use_tool(AnnotationTool::kSquare);
  EXPECT_EQ(view()->tool(), AnnotationTool::kSquare);
  send_mouse(QEvent::MouseMove, view()->page_point_to_viewport(0, {150, 450}), Qt::NoButton, Qt::NoButton);
  EXPECT_EQ(view()->viewport()->cursor().shape(), Qt::CrossCursor);
  // 4pt 미만 드래그는 아무것도 만들지 않는다
  drag(view()->page_point_to_viewport(0, {150, 450}), view()->page_point_to_viewport(0, {152, 452}));
  ASSERT_TRUE(wait_idle());
  EXPECT_EQ(view()->annotations(0)->size(), 0u);
  // 역방향 드래그도 정규화된다
  drag(view()->page_point_to_viewport(0, {300, 520}), view()->page_point_to_viewport(0, {100, 440}));
  ASSERT_TRUE(wait_annotations(0, 1));
  const auto& a = (*view()->annotations(0))[0];
  EXPECT_EQ(a.kind, engine::AnnotationKind::kSquare);
  EXPECT_NEAR(a.rect.x, 100, 1.0);
  EXPECT_NEAR(a.rect.y, 440, 1.0);
  EXPECT_NEAR(a.rect.w, 200, 1.0);
  EXPECT_NEAR(a.rect.h, 80, 1.0);
  EXPECT_FLOAT_EQ(a.border_width, 2.0f);
  EXPECT_FLOAT_EQ(a.opacity, 1.0f);
  EXPECT_TRUE(a.flags & engine::kAnnotFlagPrint);
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("사각형 추가")));
  EXPECT_TRUE(window_.isWindowModified());
  ASSERT_TRUE(window_.edit_controller()->undo());
  ASSERT_TRUE(wait_annotations(0, 0));
  // 원
  use_tool(AnnotationTool::kCircle);
  drag(view()->page_point_to_viewport(0, {320, 440}), view()->page_point_to_viewport(0, {440, 560}));
  ASSERT_TRUE(wait_annotations(0, 1));
  EXPECT_EQ((*view()->annotations(0))[0].kind, engine::AnnotationKind::kCircle);
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("원 추가")));
}

TEST_F(AnnotationTest, DefaultsApplyToNewShapesAndPenOnly) {
  open_copy("simple.pdf");
  ASSERT_TRUE(wait_annotations(0, 0));
  // 선택이 없을 때: 새 도형·펜의 기본값만 바뀐다
  window_.set_annotation_border_width(8.0);
  window_.set_annotation_opacity(128);
  EXPECT_FALSE(window_.edit_controller()->busy());
  EXPECT_DOUBLE_EQ(window_.shape_border_width(), 8.0);
  EXPECT_EQ(window_.shape_alpha(), 128);
  EXPECT_EQ(window_.width_button()->text(), QStringLiteral("8pt"));
  EXPECT_EQ(window_.opacity_button()->text(), QStringLiteral("50%"));
  use_tool(AnnotationTool::kPen);
  drag(view()->page_point_to_viewport(0, {100, 400}), view()->page_point_to_viewport(0, {200, 420}));
  ASSERT_TRUE(wait_annotations(0, 1));
  EXPECT_FLOAT_EQ((*view()->annotations(0))[0].border_width, 8.0f);
  EXPECT_NEAR((*view()->annotations(0))[0].opacity, 0.5, 0.01);
  // 마크업은 기본 불투명도의 영향을 받지 않는다 (항상 100%)
  ASSERT_TRUE(wait_text(0));
  use_tool(AnnotationTool::kHighlight);
  drag_first_word();
  ASSERT_TRUE(wait_annotations(0, 2));
  for (const auto& a : *view()->annotations(0)) {
    if (a.kind == engine::AnnotationKind::kHighlight) EXPECT_FLOAT_EQ(a.opacity, 1.0f);
  }
}

TEST_F(AnnotationTest, BorderWidthAndOpacityOnSelection) {
  open_copy("simple.pdf");
  ASSERT_TRUE(wait_annotations(0, 0));
  use_tool(AnnotationTool::kSquare);
  drag(view()->page_point_to_viewport(0, {100, 440}), view()->page_point_to_viewport(0, {300, 520}));
  ASSERT_TRUE(wait_annotations(0, 1));
  use_tool(AnnotationTool::kSelect);
  click(view()->page_point_to_viewport(0, {200, 480}));
  ASSERT_TRUE(view()->selected_annotation().has_value());
  EXPECT_TRUE(window_.width_button()->isEnabled());
  EXPECT_EQ(window_.width_button()->text(), QStringLiteral("2pt"));
  EXPECT_TRUE(window_.statusBar()->currentMessage().contains(QStringLiteral("2pt")));
  const double default_width = window_.shape_border_width();
  window_.set_annotation_border_width(4.0);
  ASSERT_TRUE(wait_idle());
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("선 두께 변경")));
  EXPECT_DOUBLE_EQ(window_.shape_border_width(), default_width) << "선택이 있으면 기본값은 그대로";
  ASSERT_TRUE(spin_until([&] { return idle() && view()->annotations(0) && (*view()->annotations(0))[0].border_width == 4.0f; }));
  EXPECT_TRUE(view()->selected_annotation().has_value()) << "편집 뒤에도 같은 NM 을 다시 고른다";
  EXPECT_EQ(window_.width_button()->text(), QStringLiteral("4pt")) << "선택 주석의 값을 보여준다";
  ASSERT_TRUE(window_.edit_controller()->undo());
  ASSERT_TRUE(spin_until([&] { return idle() && view()->annotations(0) && (*view()->annotations(0))[0].border_width == 2.0f; }));
  // 불투명도
  window_.set_annotation_opacity(128);
  ASSERT_TRUE(wait_idle());
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("불투명도 변경")));
  ASSERT_TRUE(spin_until([&] { return idle() && view()->annotations(0) && std::abs((*view()->annotations(0))[0].opacity - 0.5f) < 0.01f; }));
  EXPECT_EQ(window_.opacity_button()->text(), QStringLiteral("50%"));
  ASSERT_TRUE(window_.edit_controller()->undo());
  ASSERT_TRUE(spin_until([&] { return idle() && view()->annotations(0) && (*view()->annotations(0))[0].opacity > 0.99f; }));
  // 메모를 선택하면 두께 버튼은 비활성, 불투명도는 활성
  ASSERT_TRUE(window_.add_note(0, {400, 300}, QStringLiteral("메모")));
  ASSERT_TRUE(wait_annotations(0, 2));
  click(view()->page_point_to_viewport(0, {400, 300}));
  ASSERT_TRUE(view()->selected_annotation().has_value());
  EXPECT_EQ(view()->selected_annotation()->kind, engine::AnnotationKind::kText);
  EXPECT_FALSE(window_.width_button()->isEnabled());
  EXPECT_TRUE(window_.opacity_button()->isEnabled());
  window_.set_annotation_border_width(4.0);
  EXPECT_FALSE(window_.edit_controller()->busy()) << "메모에는 두께 명령을 보내지 않는다";
}

TEST_F(AnnotationTest, BsAnnotationWidthRecreates) {
  open_copy("annotations.pdf");
  ASSERT_TRUE(wait_annotations(0, 6));
  // 다른 프로그램의 Square(/BS /W 3): 쪽 포인트 y 172..312
  click(view()->page_point_to_viewport(0, {186, 242}));
  ASSERT_TRUE(view()->selected_annotation().has_value());
  ASSERT_EQ(view()->selected_annotation()->kind, engine::AnnotationKind::kSquare);
  EXPECT_TRUE(view()->selected_annotation()->has_bs);
  EXPECT_TRUE(window_.width_button()->isEnabled());
  const std::string id = view()->selected_annotation()->id;
  window_.set_annotation_border_width(4.0);
  EXPECT_TRUE(window_.statusBar()->currentMessage().contains(QStringLiteral("다시 만들었습니다")));
  ASSERT_TRUE(wait_idle());
  ASSERT_TRUE(spin_until([&] {
    if (!idle() || !view()->annotations(0)) return false;
    for (const auto& a : *view()->annotations(0)) if (a.kind == engine::AnnotationKind::kSquare) return !a.has_bs && a.border_width == 4.0f;
    return false;
  }));
  ASSERT_EQ(view()->annotations(0)->size(), 6u);
  const auto& sq = *std::find_if(view()->annotations(0)->begin(), view()->annotations(0)->end(),
                                 [](const engine::AnnotationInfo& a) { return a.kind == engine::AnnotationKind::kSquare; });
  EXPECT_TRUE(id.empty() || id[0] == '#' || sq.id == id) << "편집 때 NM 이 부여되고 그 뒤로 같은 NM";
  EXPECT_NEAR(sq.opacity, 0.8, 0.01);
  ASSERT_TRUE(window_.edit_controller()->undo());
  ASSERT_TRUE(wait_annotations(0, 6));
}

TEST_F(AnnotationTest, PermissionDisablesAnnotationTools) {
  ASSERT_TRUE(open("encrypted-owner-only.pdf"));  // 주석 금지(비트 6)
  EXPECT_TRUE(window_.tool_action(AnnotationTool::kSelect)->isEnabled());
  EXPECT_FALSE(window_.tool_action(AnnotationTool::kHighlight)->isEnabled());
  EXPECT_FALSE(window_.tool_action(AnnotationTool::kNote)->isEnabled());
  EXPECT_FALSE(window_.tool_action(AnnotationTool::kSquare)->isEnabled());
  EXPECT_FALSE(window_.tool_action(AnnotationTool::kCircle)->isEnabled());
  EXPECT_FALSE(window_.width_button()->isEnabled());
  EXPECT_FALSE(window_.opacity_button()->isEnabled());
  EXPECT_FALSE(window_.add_note(0, {100, 100}, QStringLiteral("x")));
  ASSERT_TRUE(open("simple.pdf"));
  EXPECT_TRUE(window_.tool_action(AnnotationTool::kHighlight)->isEnabled());
  use_tool(AnnotationTool::kHighlight);
  ASSERT_TRUE(open("encrypted-owner-only.pdf"));
  EXPECT_EQ(view()->tool(), AnnotationTool::kSelect) << "권한이 없어지면 선택 도구로 돌아간다";
}

TEST_F(AnnotationTest, SignatureWarningBeforeFirstAnnotation) {
  open_copy("signed-dummy.pdf");
  ASSERT_TRUE(spin_until([&] { return view()->annotations(0) != nullptr; }));
  const std::size_t before = view()->annotations(0)->size();
  bool warned = false;
  when_dialog("signature_warning", [&](QWidget* w) {
    warned = true;
    auto* box = qobject_cast<QMessageBox*>(w);
    for (QAbstractButton* b : box->buttons()) {
      if (box->buttonRole(b) == QMessageBox::AcceptRole) b->click();
    }
  });
  ASSERT_TRUE(window_.add_note(0, {100, 100}, QStringLiteral("서명 메모")));
  ASSERT_TRUE(wait_annotations(0, before + 1));
  EXPECT_TRUE(warned);
}

TEST_F(AnnotationTest, PrintedOutputContainsHighlight) {
  open_copy("simple.pdf");
  ASSERT_TRUE(wait_text(0));
  use_tool(AnnotationTool::kHighlight);
  drag_first_word();
  ASSERT_TRUE(wait_annotations(0, 1));
  QPrinter printer(QPrinter::HighResolution);
  printer.setOutputFormat(QPrinter::PdfFormat);
  const auto out = dir_ / "printed.pdf";
  printer.setOutputFileName(QString::fromStdString(onpdf::path_to_utf8(out)));
  ASSERT_TRUE(window_.print_with(printer));
  ASSERT_TRUE(spin_until([&] { return window_.print_job() != nullptr && !window_.print_job()->active(); }));
  auto future = engine::DocumentHandle::open(worker_, out);
  auto doc = future.get();
  ASSERT_TRUE(doc) << doc.error().message;
  engine::RenderOptions o;
  o.scale = 0.5f;
  auto bmp = doc->render_page(0, o).get();
  ASSERT_TRUE(bmp);
  int yellow = 0;
  for (int y = 0; y < bmp->height; ++y) {
    for (int x = 0; x < bmp->width; ++x) {
      const auto* p = bmp->pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(bmp->stride) +
                      static_cast<std::size_t>(x) * 4;
      if (p[2] > 200 && p[1] > 200 && p[0] < 80) ++yellow;
    }
  }
  EXPECT_GT(yellow, 50) << "하이라이트(Print 플래그)가 인쇄 출력에 있어야 한다";
}

}  // namespace
}  // namespace onpdf::app
