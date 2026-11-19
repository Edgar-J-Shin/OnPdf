// 이미지 도구 (#49, offscreen): 파일 → 놓기·선택, 드롭, 이동, 모서리 핸들 크기(비율·Shift), 삭제·취소, 기존 이미지, 실패 안내, 권한
#include "document_view.h"
#include "image_import.h"
#include "main_window.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/platform/path.h"
#include "ui_dispatcher.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QImage>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QStatusBar>
#include <QTimer>

#include <filesystem>
#include <fstream>

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

class ImageToolTest : public ::testing::Test {
 protected:
  ImageToolTest() : worker_(options()), window_(worker_, dispatcher_) {
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
  std::filesystem::path open_copy(const char* name) {
    const auto copy = dir_ / name;
    std::filesystem::copy_file(testdata(name), copy);
    EXPECT_TRUE(open_path(copy));
    return copy;
  }
  bool idle() { return !window_.edit_controller()->busy() && !window_.saving(); }
  bool wait_idle() { return spin_until([&] { return idle(); }); }
  std::vector<engine::PageObjectInfo> images(int page) {
    std::vector<engine::PageObjectInfo> out;
    if (const auto* list = view()->page_objects(page)) {
      for (const auto& o : *list) if (o.kind == engine::ObjectKind::kImage) out.push_back(o);
    }
    return out;
  }
  bool wait_images(int page, std::size_t n) {
    return spin_until([&] { return idle() && view()->page_objects(page) != nullptr && images(page).size() == n; });
  }
  // 왼쪽 절반 빨강(불투명), 오른쪽 절반 투명
  QString write_png(const char* name, int w = 120, int h = 80) {
    QImage image(w, h, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.fillRect(0, 0, w / 2, h, Qt::red);
    p.end();
    const QString path = QString::fromStdString(onpdf::path_to_utf8(dir_ / name));
    EXPECT_TRUE(image.save(path, "PNG"));
    return path;
  }
  QString write_jpeg(const char* name, int w = 160, int h = 100) {
    QImage image(w, h, QImage::Format_RGB32);
    image.fill(QColor(40, 160, 40));
    const QString path = QString::fromStdString(onpdf::path_to_utf8(dir_ / name));
    EXPECT_TRUE(image.save(path, "JPEG"));
    return path;
  }
  void send_mouse(QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons,
                  Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QMouseEvent ev(type, pos, view()->viewport()->mapToGlobal(pos.toPoint()), button, buttons, mods);
    QApplication::sendEvent(view()->viewport(), &ev);
  }
  void click(QPointF at) {
    send_mouse(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    send_mouse(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
  }
  void drag(QPointF from, QPointF to, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    send_mouse(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, mods);
    send_mouse(QEvent::MouseMove, (from + to) / 2, Qt::NoButton, Qt::LeftButton, mods);
    send_mouse(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton, mods);
    send_mouse(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, mods);
  }
  void settle_clicks() {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 600) QApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  QPointF vp(int page, double x, double y) { return view()->page_point_to_viewport(page, {x, y}); }

  ScopedTempDir dir_;
  UiDispatcher dispatcher_;
  engine::PdfiumWorker worker_;
  MainWindow window_;
};

TEST_F(ImageToolTest, PdfAndSvgAreNotImages) {
  EXPECT_FALSE(looks_like_image_file(QStringLiteral("/tmp/a.pdf")));
  EXPECT_FALSE(looks_like_image_file(QStringLiteral("/tmp/a.PDF")));
  EXPECT_TRUE(looks_like_image_file(QStringLiteral("/tmp/a.png")));
  EXPECT_TRUE(looks_like_image_file(QStringLiteral("/tmp/a.jpg")));
  EXPECT_FALSE(image_file_filter().contains(QStringLiteral("*.pdf")));
}

TEST_F(ImageToolTest, PlaceImageRespectsPageAndScale) {
  const engine::PageSize letter{612, 792};
  Rect r = place_image(letter, {100, 100}, 120, 80, false);
  EXPECT_DOUBLE_EQ(r.x, 100);
  EXPECT_DOUBLE_EQ(r.w, 120) << "1px = 1pt";
  r = place_image(letter, {0, 0}, 3000, 2000, false);
  EXPECT_NEAR(r.w, 612 * 0.8, 0.01) << "쪽 폭 80% 상한";
  EXPECT_NEAR(r.h, 612 * 0.8 * 2000 / 3000, 0.01);
  r = place_image(letter, {600, 780}, 120, 80, false);
  EXPECT_LE(r.right(), 612.0) << "쪽 안으로";
  EXPECT_LE(r.bottom(), 792.0);
  r = place_image(letter, {306, 396}, 100, 100, true);
  EXPECT_DOUBLE_EQ(r.x, 256);
}

TEST_F(ImageToolTest, ToolPlacesPngAndJpegAndSelectsNewImage) {
  open_copy("simple.pdf");
  ASSERT_TRUE(wait_images(0, 0));
  ASSERT_TRUE(window_.tool_action(AnnotationTool::kImage)->isEnabled());
  window_.tool_action(AnnotationTool::kImage)->trigger();
  const QString png = write_png("alpha.png");
  ASSERT_TRUE(window_.add_image_file(png, 0, {100, 400}, false));
  ASSERT_TRUE(wait_images(0, 1));
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("이미지 추가")));
  const auto placed = images(0)[0];
  EXPECT_NEAR(placed.bbox.x, 100, 0.5);
  EXPECT_NEAR(placed.bbox.w, 120, 0.5);
  EXPECT_EQ(placed.pixel_width, 120);
  ASSERT_TRUE(spin_until([&] { return view()->selected_object().has_value(); })) << "넣은 이미지를 고른다";
  EXPECT_EQ(view()->tool(), AnnotationTool::kSelect);
  EXPECT_EQ(view()->selected_object()->kind, engine::ObjectKind::kImage);
  EXPECT_TRUE(window_.statusBar()->currentMessage().contains(QStringLiteral("120×80 px")));
  // JPEG 은 원본 그대로 (DCTDecode)
  const QString jpg = write_jpeg("photo.jpg");
  ASSERT_TRUE(window_.add_image_file(jpg, 0, {300, 500}, false));
  ASSERT_TRUE(wait_images(0, 2));
  // 필터는 저장 전에도 JPEG 은 DCTDecode
  EXPECT_EQ(images(0)[1].filter, "DCTDecode");
  // 실행 취소 → 재실행
  ASSERT_TRUE(window_.edit_controller()->undo());
  ASSERT_TRUE(wait_images(0, 1));
  ASSERT_TRUE(window_.edit_controller()->redo());
  ASSERT_TRUE(wait_images(0, 2));
}

TEST_F(ImageToolTest, MoveResizeDeleteAndUndo) {
  open_copy("simple.pdf");
  ASSERT_TRUE(wait_images(0, 0));
  ASSERT_TRUE(window_.add_image_file(write_png("a.png", 100, 50), 0, {100, 400}, false));
  ASSERT_TRUE(wait_images(0, 1));
  ASSERT_TRUE(spin_until([&] { return view()->selected_object().has_value(); }));
  // 몸통 드래그 → 이동
  settle_clicks();
  drag(vp(0, 150, 425), vp(0, 190, 455));
  ASSERT_TRUE(spin_until([&] { return wait_idle() && !images(0).empty() && std::abs(images(0)[0].bbox.x - 140) < 1.0; }));
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("객체 이동")));
  ASSERT_TRUE(spin_until([&] { return view()->selected_object().has_value(); }));
  // 우하 핸들 → 비율 유지 크기 조절 (100×50 → 폭 200)
  const auto b = images(0)[0].bbox;
  settle_clicks();
  drag(vp(0, b.right(), b.bottom()), vp(0, b.right() + 100, b.bottom() + 10));
  ASSERT_TRUE(spin_until([&] { return wait_idle() && !images(0).empty() && std::abs(images(0)[0].bbox.w - 200) < 1.5; }));
  EXPECT_NEAR(images(0)[0].bbox.h, 100, 1.5) << "비율 유지";
  EXPECT_NEAR(images(0)[0].bbox.x, b.x, 0.5) << "반대 모서리 고정";
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("크기 변경")));
  // Shift → 자유 비율
  ASSERT_TRUE(spin_until([&] { return view()->selected_object().has_value(); }));
  const auto c = images(0)[0].bbox;
  settle_clicks();
  drag(vp(0, c.right(), c.bottom()), vp(0, c.right() - 50, c.bottom() + 40), Qt::ShiftModifier);
  ASSERT_TRUE(spin_until([&] { return wait_idle() && !images(0).empty() && std::abs(images(0)[0].bbox.w - 150) < 1.5; }));
  EXPECT_NEAR(images(0)[0].bbox.h, 140, 1.5);
  // Delete → 취소
  ASSERT_TRUE(spin_until([&] { return view()->selected_object().has_value(); }));
  QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
  QApplication::sendEvent(view(), &del);
  ASSERT_TRUE(wait_images(0, 0));
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("삭제")));
  ASSERT_TRUE(window_.edit_controller()->undo());
  ASSERT_TRUE(wait_images(0, 1));
  EXPECT_NEAR(images(0)[0].bbox.w, 150, 1.5) << "같은 객체가 돌아온다";
}

TEST_F(ImageToolTest, ExistingImageCanBeSelectedAndMoved) {
  open_copy("scanned.pdf");
  ASSERT_TRUE(wait_images(0, 1));
  const auto foreign = images(0)[0];
  EXPECT_FALSE(foreign.marked);
  click(vp(0, foreign.bbox.x + foreign.bbox.w / 2, foreign.bbox.y + foreign.bbox.h / 2));
  ASSERT_TRUE(view()->selected_object().has_value());
  EXPECT_EQ(view()->selected_object()->kind, engine::ObjectKind::kImage);
  // 쪽 전체를 덮는 스캔 이미지: 우하 핸들로 먼저 줄인 뒤(자유 비율) 옮긴다
  settle_clicks();
  drag(vp(0, foreign.bbox.right(), foreign.bbox.bottom()), vp(0, foreign.bbox.right() - 200, foreign.bbox.bottom() - 200),
       Qt::ShiftModifier);
  ASSERT_TRUE(spin_until([&] { return wait_idle() && !images(0).empty() && images(0)[0].marked; })) << "첫 편집에서 마크가 붙는다";
  EXPECT_NEAR(images(0)[0].bbox.w, foreign.bbox.w - 200, 1.5);
  ASSERT_TRUE(spin_until([&] { return view()->selected_object().has_value(); }));
  const auto shrunk = images(0)[0].bbox;
  settle_clicks();
  drag(vp(0, shrunk.x + 50, shrunk.y + 50), vp(0, shrunk.x + 90, shrunk.y + 70));
  ASSERT_TRUE(spin_until([&] { return wait_idle() && !images(0).empty() && std::abs(images(0)[0].bbox.x - (shrunk.x + 40)) < 1.0; }));
  ASSERT_TRUE(spin_until([&] {
    if (!idle()) { return false; }
    if (window_.edit_controller()->can_undo()) { window_.edit_controller()->undo(); return false; }
    return !images(0).empty() && std::abs(images(0)[0].bbox.w - foreign.bbox.w) < 1.0;
  })) << "실행 취소로 원래 크기·자리";
}

TEST_F(ImageToolTest, UnreadableFileIsExplainedAndPermissionGates) {
  open_copy("simple.pdf");
  ASSERT_TRUE(wait_idle());
  const auto bogus = dir_ / "broken.png";
  { std::ofstream(bogus) << "not an image"; }
  bool warned = false;
  when_dialog("image_open_failed", [&](QWidget* w) {
    warned = true;
    static_cast<QMessageBox*>(w)->button(QMessageBox::Ok)->click();
  });
  EXPECT_FALSE(window_.add_image_file(QString::fromStdString(onpdf::path_to_utf8(bogus)), 0, {10, 10}, false));
  EXPECT_TRUE(warned);
  EXPECT_FALSE(window_.isWindowModified());
  ASSERT_TRUE(open_path(testdata("encrypted-owner-only.pdf")));  // 내용 수정 금지
  ASSERT_TRUE(wait_idle());
  EXPECT_FALSE(window_.tool_action(AnnotationTool::kImage)->isEnabled());
  EXPECT_FALSE(window_.add_image_file(write_png("x.png"), 0, {10, 10}, false));
}

// 클립보드 붙여넣기 (#49): 현재 쪽 가운데, 투명도 유지
TEST_F(ImageToolTest, PasteFromClipboardCentersOnCurrentPage) {
  open_copy("simple.pdf");
  ASSERT_TRUE(wait_images(0, 0));
  QImage image(100, 60, QImage::Format_ARGB32);
  image.fill(Qt::transparent);
  QApplication::clipboard()->setImage(image);
  ASSERT_TRUE(window_.paste_action()->isEnabled());
  window_.paste_action()->trigger();
  ASSERT_TRUE(wait_images(0, 1));
  const auto pasted = images(0)[0];
  EXPECT_NEAR(pasted.bbox.x + pasted.bbox.w / 2, 306, 1.0) << "쪽 가운데";
  EXPECT_NEAR(pasted.bbox.y + pasted.bbox.h / 2, 396, 1.0);
  EXPECT_NEAR(pasted.bbox.w, 100, 0.5);
  // 이미지가 없으면 안내만
  QApplication::clipboard()->setText(QStringLiteral("text only"));
  EXPECT_FALSE(window_.paste_image());
  EXPECT_TRUE(window_.statusBar()->currentMessage().contains(QStringLiteral("클립보드")));
}

// 화살표 키 (#49): 1pt·Shift 10pt, 연타는 실행 취소 한 단계, 선택 없으면 이동 없음
TEST_F(ImageToolTest, ArrowKeysNudgeAndMergeIntoOneUndo) {
  open_copy("simple.pdf");
  ASSERT_TRUE(wait_images(0, 0));
  ASSERT_TRUE(window_.add_image_file(write_png("n.png", 40, 40), 0, {200, 300}, false));
  ASSERT_TRUE(wait_images(0, 1));
  ASSERT_TRUE(spin_until([&] { return view()->selected_object().has_value(); }));
  auto press = [&](Qt::Key key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QKeyEvent ev(QEvent::KeyPress, key, mods);
    QApplication::sendEvent(view(), &ev);
  };
  press(Qt::Key_Right);
  press(Qt::Key_Right);
  press(Qt::Key_Down, Qt::ShiftModifier);
  const bool moved = spin_until([&] {
    return idle() && !images(0).empty() && std::abs(images(0)[0].bbox.x - 202) < 0.5 && std::abs(images(0)[0].bbox.y - 310) < 0.5;
  });
  ASSERT_TRUE(moved) << "bbox " << images(0)[0].bbox.x << "," << images(0)[0].bbox.y << " undo "
                     << window_.undo_action()->text().toStdString();
  EXPECT_TRUE(window_.undo_action()->text().contains(QStringLiteral("객체 이동")));
  ASSERT_TRUE(window_.edit_controller()->undo());
  ASSERT_TRUE(spin_until([&] {
    return idle() && !images(0).empty() && std::abs(images(0)[0].bbox.x - 200) < 0.5 && std::abs(images(0)[0].bbox.y - 300) < 0.5;
  })) << "연타 3번이 한 번의 실행 취소로";
  // 선택 해제 뒤 화살표는 객체를 옮기지 않는다
  QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QApplication::sendEvent(view(), &esc);
  press(Qt::Key_Right);
  ASSERT_TRUE(wait_idle());
  EXPECT_NEAR(images(0)[0].bbox.x, 200, 0.5);
}

}  // namespace
}  // namespace onpdf::app
