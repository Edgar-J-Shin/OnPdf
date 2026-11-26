// 기존 텍스트 줄 수정 (#65) 앱: hover 상자, 클릭 선택·상태 표시줄, 두 번 클릭 → 수정 창 → 대체 글꼴
// 안내·새 상자 선택, Delete → 삭제, 실행 취소, 불가 줄 안내, 권한·서명 게이트
#include "onpdf/core/engine/document_handle.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/engine/qpdf_tools.h"
#include "onpdf/core/platform/path.h"
#include "onpdf/edit/font_catalog.h"

#include "../test_support.h"
#include "annotation_dialog.h"
#include "document_view.h"
#include "main_window.h"
#include "ui_dispatcher.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QTimer>
#include <filesystem>
#include <gtest/gtest.h>

namespace onpdf::app {
namespace {

using onpdf::test::ScopedTempDir;
using onpdf::test::testdata;

template <class Pred>
// 느린 CI 러너(Windows)에서 줄 수정·대화상자 테스트가 10초를 넘긴 적이 있다 (#105) → print_test
// 보다 넉넉히
bool spin_until(Pred pred, int timeout_ms = 30000) {
  QElapsedTimer timer;
  timer.start();
  while (!pred()) {
    if (timer.elapsed() > timeout_ms) return false;
    QApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  return true;
}

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

class TextLineToolTest : public ::testing::Test {
 protected:
  TextLineToolTest() : worker_(options()), window_(worker_, dispatcher_) {
    window_.resize(900, 900);
    window_.show();
    EXPECT_TRUE(window_.set_text_font(
        QString::fromStdString(onpdf::path_to_utf8(testdata("fonts") / "NotoSansKR-subset.ttf"))));
    window_.set_font_catalog(std::make_shared<const edit::FontCatalog>(
        edit::FontCatalog::from_entries({})));  // 시스템 글꼴 배제 (결정적)
  }
  static engine::PdfiumWorker::Options options() {
    engine::PdfiumWorker::Options o;
    o.use_system_fonts = false;
    return o;
  }
  DocumentView* view() { return window_.document_view(); }
  std::filesystem::path open_copy(const char* name) {
    const auto copy = dir_ / name;
    std::filesystem::copy_file(testdata(name), copy);
    const QString file = QString::fromStdString(onpdf::path_to_utf8(copy.filename()));
    window_.open_file(QString::fromStdString(onpdf::path_to_utf8(copy)));
    EXPECT_TRUE(
        spin_until([&] { return window_.windowTitle().contains(file) && view()->has_document(); }));
    return copy;
  }
  bool idle() { return !window_.edit_controller()->busy() && !window_.saving(); }
  bool wait_idle() {
    return spin_until([&] { return idle(); });
  }
  bool wait_lines(int page) {
    return spin_until([&] { return idle() && view()->text_lines(page) != nullptr; });
  }
  const engine::TextLineInfo* line_containing(int page, const QString& needle) {
    const auto* lines = view()->text_lines(page);
    if (lines == nullptr) return nullptr;
    for (const auto& l : *lines) {
      if (QString::fromStdString(l.text).contains(needle)) return &l;
    }
    return nullptr;
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
  void send_mouse(QEvent::Type type, QPointF pos, Qt::MouseButton button,
                  Qt::MouseButtons buttons) {
    QMouseEvent ev(type, pos, view()->viewport()->mapToGlobal(pos.toPoint()), button, buttons,
                   Qt::NoModifier);
    QApplication::sendEvent(view()->viewport(), &ev);
  }
  void click(QPointF at) {
    send_mouse(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    send_mouse(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
  }
  QPointF center_of(int page, const engine::TextLineInfo& line) {
    return view()->page_point_to_viewport(
        page, {line.bbox.x + line.bbox.w / 2, line.bbox.y + line.bbox.h / 2});
  }

  ScopedTempDir dir_;
  UiDispatcher dispatcher_;
  engine::PdfiumWorker worker_;
  MainWindow window_;
};

TEST_F(TextLineToolTest, HoverSelectEditDeleteUndo) {
  open_copy("text-lines-basic.pdf");
  ASSERT_TRUE(wait_lines(0));
  ASSERT_TRUE(spin_until([&] { return view()->text_index(0) != nullptr; }));
  const engine::TextLineInfo* hello = line_containing(0, QStringLiteral("Hello world line"));
  ASSERT_NE(hello, nullptr);
  const engine::TextLineInfo hello_info = *hello;
  // 선택 도구에서는 글자 클릭이 글자 선택이다 (줄 선택 없음)
  click(center_of(0, hello_info));
  EXPECT_FALSE(view()->selected_line().has_value());
  // 텍스트 도구: hover → I 빔 + 점선 상자
  window_.tool_action(AnnotationTool::kText)->trigger();
  ASSERT_EQ(view()->tool(), AnnotationTool::kText);
  send_mouse(QEvent::MouseMove, center_of(0, hello_info), Qt::NoButton, Qt::NoButton);
  EXPECT_EQ(view()->viewport()->cursor().shape(), Qt::IBeamCursor);
  // 클릭 선택 → 상태 표시줄
  click(center_of(0, hello_info));
  ASSERT_TRUE(view()->selected_line().has_value());
  EXPECT_EQ(view()->selected_line()->info.id, hello_info.id);
  EXPECT_TRUE(window_.statusBar()->currentMessage().contains(QStringLiteral("Helvetica")));
  EXPECT_TRUE(window_.delete_annotation_action()->isEnabled());
  // Enter → 수정 창 → 대체 글꼴(비임베딩 Helvetica)로 교체 + 안내 + 새 상자 선택
  when_dialog("text_input", [](QWidget* w) {
    auto* edit = w->findChild<QPlainTextEdit*>(QStringLiteral("note_text"));
    ASSERT_NE(edit, nullptr);
    edit->setPlainText(QStringLiteral("Hello edited line"));
    static_cast<AnnotationDialog*>(w)->accept();
  });
  QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
  QApplication::sendEvent(view(), &enter);
  ASSERT_TRUE(wait_text_contains(0, QStringLiteral("Hello edited line")));
  EXPECT_FALSE(QString::fromStdString(view()->text_index(0)->all_text())
                   .contains(QStringLiteral("Hello world line")));
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("줄 수정")));
  EXPECT_TRUE(window_.isWindowModified());
  ASSERT_TRUE(spin_until([&] { return view()->selected_object().has_value(); }))
      << "새 텍스트 상자가 선택된다";
  EXPECT_FALSE(view()->selected_line().has_value());
  EXPECT_TRUE(window_.statusBar()->currentMessage().contains(QStringLiteral("NotoSansKR-subset")))
      << "대체 글꼴 안내 (실제 글꼴 이름)";
  // 실행 취소 → 원래 줄
  ASSERT_TRUE(window_.edit_controller()->undo());
  ASSERT_TRUE(wait_text_contains(0, QStringLiteral("Hello world line")));
  ASSERT_TRUE(wait_lines(0));
  window_.tool_action(AnnotationTool::kText)->trigger();
  // Delete 로 다른 줄 삭제
  const engine::TextLineInfo* gone = line_containing(0, QStringLiteral("Delete me entirely"));
  ASSERT_NE(gone, nullptr);
  const engine::TextLineInfo gone_info = *gone;
  click(center_of(0, gone_info));
  ASSERT_TRUE(view()->selected_line().has_value());
  QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
  QApplication::sendEvent(view(), &del);
  ASSERT_TRUE(wait_text_missing(0, QStringLiteral("Delete me entirely")));
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("줄 삭제")));
  EXPECT_FALSE(view()->selected_line().has_value()) << "편집 뒤 줄 선택은 해제된다";
  ASSERT_TRUE(window_.edit_controller()->undo());
  ASSERT_TRUE(wait_text_contains(0, QStringLiteral("Delete me entirely")));
}

// #69: 줄을 고르고 크기 콤보·색 팔레트 → 그 줄만 (원본 글꼴 유지, 기본값 불변), ofont 상자도 원본
// 글꼴 유지
TEST_F(TextLineToolTest, SizeComboAndPaletteApplyToSelectedLine) {
  open_copy("text-lines-type3.pdf");
  ASSERT_TRUE(wait_lines(0));
  ASSERT_TRUE(spin_until([&] { return view()->text_index(0) != nullptr; }));
  const engine::TextLineInfo* korean = line_containing(0, QStringLiteral("한글 텍스트 줄"));
  ASSERT_NE(korean, nullptr);
  const engine::TextLineInfo info = *korean;
  window_.tool_action(AnnotationTool::kText)->trigger();
  // 불가 줄(보이지 않는 글)을 고른 채 크기·색을 바꾸면 안내만, 기본값·문서 불변 (리뷰 W2)
  const engine::TextLineInfo* invisible =
      line_containing(0, QStringLiteral("Invisible OCR layer text"));
  ASSERT_NE(invisible, nullptr);
  ASSERT_FALSE(invisible->editable);
  const double default_size = window_.text_size();
  const engine::Color default_color = window_.annotation_color();
  click(center_of(0, *invisible));
  ASSERT_TRUE(view()->selected_line().has_value());
  window_.set_text_size(36);
  window_.set_annotation_color({0, 0, 255, 255});
  ASSERT_TRUE(wait_idle());
  EXPECT_DOUBLE_EQ(window_.text_size(), default_size);
  EXPECT_EQ(window_.annotation_color().b, default_color.b);
  EXPECT_FALSE(window_.edit_controller()->can_undo());
  EXPECT_FALSE(window_.statusBar()->currentMessage().isEmpty()) << "불가 사유 안내";
  click(center_of(0, info));
  ASSERT_TRUE(view()->selected_line().has_value());
  EXPECT_EQ(view()->selected_line()->info.id, info.id);
  EXPECT_EQ(window_.text_size_combo()->currentData().toInt(), 18)
      << "콤보는 선택한 줄의 크기를 비춘다";
  window_.set_text_size(36);
  ASSERT_TRUE(spin_until([&] {
    return idle() && view()->text_objects(0) && view()->text_objects(0)->size() == 1 &&
           std::abs((*view()->text_objects(0))[0].font_size - 36.0) < 0.05;
  }));
  EXPECT_DOUBLE_EQ(window_.text_size(), default_size) << "기본값은 그대로";
  EXPECT_TRUE((*view()->text_objects(0))[0].original_font) << "원본 글꼴 유지";
  EXPECT_EQ((*view()->text_objects(0))[0].text, "한글 텍스트 줄");
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("줄 서식 변경")));
  ASSERT_TRUE(spin_until([&] { return view()->selected_text_box().has_value(); }))
      << "선택 도구로 돌아가 새 상자 선택";
  EXPECT_EQ(view()->tool(), AnnotationTool::kSelect);
  // ofont 상자의 색 → 원본 글꼴 유지 (번들 글꼴로 바뀌지 않는다)
  window_.set_annotation_color({0, 0, 255, 255});
  ASSERT_TRUE(spin_until([&] {
    return idle() && view()->text_objects(0) && view()->text_objects(0)->size() == 1 &&
           (*view()->text_objects(0))[0].color.b == 255;
  }));
  EXPECT_TRUE((*view()->text_objects(0))[0].original_font);
  EXPECT_NEAR((*view()->text_objects(0))[0].font_size, 36.0, 0.05);
  EXPECT_NE(window_.annotation_color().b, 255) << "기본 색은 그대로";
  // 같은 상자의 서식 연타(크기 → 색)는 한 단계로 합쳐져 실행 취소 한 번에 원래 줄로 (#79)
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("줄 서식 변경")));
  ASSERT_TRUE(window_.edit_controller()->undo());
  ASSERT_TRUE(spin_until(
      [&] { return idle() && view()->text_objects(0) && view()->text_objects(0)->empty(); }));
  EXPECT_FALSE(window_.edit_controller()->can_undo()) << "합쳐져 한 단계뿐";
  ASSERT_TRUE(wait_lines(0));
  EXPECT_NE(line_containing(0, QStringLiteral("한글 텍스트 줄")), nullptr);
}

// #73: 카탈로그에 원본 글꼴과 같은 이름이 있으면 그 글꼴(CFF 포함)로, 없으면 번들로
TEST_F(TextLineToolTest, FallbackFontComesFromCatalog) {
  edit::FontEntry helvetica;
  helvetica.path = testdata("fonts") / "NotoSansCJKkr-subset.otf";
  helvetica.family =
      "Helvetica";  // 카탈로그 이름(매칭 기준)은 BaseFont 와 같게, 실제 파일은 CFF 서브셋
  helvetica.style = "Regular";
  helvetica.cff = true;
  helvetica.hangul = true;
  helvetica.glyph_count = 484;
  window_.set_font_catalog(
      std::make_shared<const edit::FontCatalog>(edit::FontCatalog::from_entries({helvetica})));
  open_copy("text-lines-basic.pdf");
  ASSERT_TRUE(wait_lines(0));
  ASSERT_TRUE(spin_until([&] { return view()->text_index(0) != nullptr; }));
  const engine::TextLineInfo* hello = line_containing(0, QStringLiteral("Hello world line"));
  ASSERT_NE(hello, nullptr);
  EXPECT_EQ(hello->font_name, "Helvetica");
  const engine::TextLineInfo hello_info = *hello;
  window_.tool_action(AnnotationTool::kText)->trigger();
  click(center_of(0, hello_info));
  ASSERT_TRUE(view()->selected_line().has_value());
  when_dialog("text_input", [](QWidget* w) {
    auto* edit = w->findChild<QPlainTextEdit*>(QStringLiteral("note_text"));
    ASSERT_NE(edit, nullptr);
    edit->setPlainText(QStringLiteral("Hello 한글 CID"));
    static_cast<AnnotationDialog*>(w)->accept();
  });
  QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
  QApplication::sendEvent(view(), &enter);
  ASSERT_TRUE(wait_text_contains(0, QStringLiteral("Hello 한글 CID")));
  ASSERT_TRUE(spin_until([&] { return view()->selected_text_box().has_value(); }));
  EXPECT_EQ(view()->selected_text_box()->font_name, "NotoSansCJKkr-subset")
      << "카탈로그 글꼴(CFF)로 넣었다";
  EXPECT_TRUE(
      window_.statusBar()->currentMessage().contains(QStringLiteral("NotoSansCJKkr-subset")));
  // 같은 상자의 크기 변경도 같은 글꼴을 유지한다 (번들로 바뀌지 않는다)
  window_.set_text_size(30);
  ASSERT_TRUE(spin_until([&] {
    return idle() && view()->text_objects(0) && !view()->text_objects(0)->empty() &&
           std::abs((*view()->text_objects(0))[0].font_size - 30.0) < 0.05;
  }));
  EXPECT_EQ((*view()->text_objects(0))[0].font_name, "NotoSansCJKkr-subset");
  // 저장하면 CFF 글꼴이 규격(FontFile3)으로 바뀐다
  const auto saved_path = dir_ / "saved.pdf";
  ASSERT_TRUE(window_.save_to(QString::fromStdString(onpdf::path_to_utf8(saved_path))));
  ASSERT_TRUE(spin_until([&] { return idle() && std::filesystem::exists(saved_path); }));
  auto census = engine::QpdfTools::font_census(saved_path);
  ASSERT_TRUE(census) << census.error().message;
  EXPECT_EQ(census->fontfile2_otto, 0);
  EXPECT_EQ(census->fontfile3_opentype, 1);
  // 카탈로그를 비우면 번들로
  ASSERT_TRUE(window_.edit_controller()->undo());
  ASSERT_TRUE(wait_idle());
  ASSERT_TRUE(window_.edit_controller()->undo());
  ASSERT_TRUE(wait_text_contains(0, QStringLiteral("Hello world line")));
  ASSERT_TRUE(wait_lines(0));
  window_.set_font_catalog(
      std::make_shared<const edit::FontCatalog>(edit::FontCatalog::from_entries({})));
  window_.tool_action(AnnotationTool::kText)->trigger();
  const engine::TextLineInfo* again = line_containing(0, QStringLiteral("Hello world line"));
  ASSERT_NE(again, nullptr);
  const engine::TextLineInfo again_info = *again;
  click(center_of(0, again_info));
  ASSERT_TRUE(view()->selected_line().has_value());
  when_dialog("text_input", [](QWidget* w) {
    auto* edit = w->findChild<QPlainTextEdit*>(QStringLiteral("note_text"));
    ASSERT_NE(edit, nullptr);
    edit->setPlainText(QStringLiteral("Hello 한글 again"));
    static_cast<AnnotationDialog*>(w)->accept();
  });
  QApplication::sendEvent(view(), &enter);
  ASSERT_TRUE(wait_text_contains(0, QStringLiteral("Hello 한글 again")));
  ASSERT_TRUE(spin_until([&] { return view()->selected_text_box().has_value(); }));
  EXPECT_EQ(view()->selected_text_box()->font_name, "NotoSansKR-subset") << "카탈로그가 비면 번들";
  EXPECT_TRUE(window_.has_font_catalog()) << "주입한 카탈로그가 스캔 결과로 덮이지 않는다";
}

TEST_F(TextLineToolTest, UneditableLineAndSignedDocumentAreExplained) {
  open_copy("text-lines-form.pdf");
  ASSERT_TRUE(wait_lines(0));
  const engine::TextLineInfo* inside = line_containing(0, QStringLiteral("Inside form first line"));
  ASSERT_NE(inside, nullptr);
  EXPECT_TRUE(inside->editable);
  EXPECT_EQ(line_containing(0, QStringLiteral("Nested form line")), nullptr)
      << "2단계 Form 안의 줄은 목록에 없다";
  // 서명 문서: 선택은 되지만 안내만
  open_copy("signed-dummy.pdf");
  ASSERT_TRUE(wait_lines(0));
  const auto* lines = view()->text_lines(0);
  ASSERT_NE(lines, nullptr);
  if (!lines->empty()) {
    const engine::TextLineInfo first = lines->front();
    window_.tool_action(AnnotationTool::kText)->trigger();
    click(center_of(0, first));
    ASSERT_TRUE(view()->selected_line().has_value());
    EXPECT_TRUE(window_.statusBar()->currentMessage().contains(QStringLiteral("서명")))
        << window_.statusBar()->currentMessage().toStdString()
        << " sig=" << window_.document_view()->has_document();
    QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(view(), &del);
    ASSERT_TRUE(wait_idle());
    EXPECT_FALSE(window_.edit_controller()->can_undo()) << "서명 문서는 줄을 지우지 않는다";
  }
}

TEST_F(TextLineToolTest, PermissionGatesLineEditing) {
  open_copy("perm-no-assemble.pdf");
  ASSERT_TRUE(wait_lines(0));
  const auto* lines = view()->text_lines(0);
  ASSERT_NE(lines, nullptr);
  ASSERT_FALSE(lines->empty());
  const engine::TextLineInfo first = lines->front();
  send_mouse(QEvent::MouseMove, center_of(0, first), Qt::NoButton, Qt::NoButton);
  if (!window_.tool_action(AnnotationTool::kText)->isEnabled()) {  // 내용 수정 권한 없음
    EXPECT_NE(view()->viewport()->cursor().shape(), Qt::IBeamCursor)
        << "권한이 없으면 hover 하지 않는다";
    click(center_of(0, first));
    QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(view(), &del);
    ASSERT_TRUE(wait_idle());
    EXPECT_FALSE(window_.edit_controller()->can_undo());
  }
}

}  // namespace
}  // namespace onpdf::app
