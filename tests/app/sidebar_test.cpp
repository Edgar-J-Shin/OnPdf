#include "document_view.h"
#include "main_window.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/platform/path.h"
#include "outline_panel.h"
#include "thumbnail_panel.h"
#include "ui_dispatcher.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QKeyEvent>
#include <QListView>
#include <QWheelEvent>
#include <QStatusBar>
#include <QLabel>
#include <QMouseEvent>
#include <QScreen>
#include <QScrollBar>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QTreeView>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <set>

namespace onpdf::app {
namespace {

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

void settle(int ms) {
  QEventLoop loop;
  QTimer::singleShot(ms, &loop, &QEventLoop::quit);
  loop.exec();
}

class SidebarTest : public ::testing::Test {
 protected:
  SidebarTest() : worker_(options()), window_(worker_, dispatcher_) {
    window_.resize(900, 900);
    window_.show();
  }
  static engine::PdfiumWorker::Options options() {
    engine::PdfiumWorker::Options o;
    o.use_system_fonts = false;
    return o;
  }
  // 문서가 열리고 목차 응답까지 처리될 때까지 (목차가 없으면 썸네일 탭이 유지된다)
  bool open(const char* name, int expected_outline_rows) {
    window_.open_file(QString::fromStdString(onpdf::path_to_utf8(testdata(name))));
    return spin_until([&] {
      return window_.document_view()->has_document() && outline().row_count() == expected_outline_rows &&
             (expected_outline_rows == 0 || window_.sidebar()->currentWidget() == window_.outline_panel());
    });
  }
  OutlinePanel& outline() { return *window_.outline_panel(); }
  ThumbnailPanel& thumbs() { return *window_.thumbnail_panel(); }
  int find_item(const std::string& title) {
    const auto* o = outline().outline();
    if (o == nullptr) return -1;
    for (int i = 0; i < o->size(); ++i) {
      if (o->items[static_cast<std::size_t>(i)].title == title) return i;
    }
    return -1;
  }
  std::string highlighted_title() {
    const int i = outline().highlighted_item();
    return i < 0 ? std::string() : outline().outline()->items[static_cast<std::size_t>(i)].title;
  }

  UiDispatcher dispatcher_;
  engine::PdfiumWorker worker_;
  MainWindow window_;
};

TEST_F(SidebarTest, OutlineTabShowsKoreanBookmarks) {
  ASSERT_TRUE(open("bookmarks.pdf", 15));
  EXPECT_EQ(window_.sidebar()->currentWidget(), window_.outline_panel());
  EXPECT_TRUE(window_.sidebar()->isVisible());
  auto* model = outline().tree()->model();
  ASSERT_EQ(model->rowCount(), 3);  // 장 3개
  EXPECT_EQ(model->index(0, 0).data().toString(), QStringLiteral("제 1 장 (page 1)"));
  // /Count > 0 인 장은 펼쳐져 있고, 항은 접혀 있다
  const QModelIndex ch1 = model->index(0, 0);
  EXPECT_TRUE(outline().tree()->isExpanded(ch1));
  EXPECT_EQ(model->rowCount(ch1), 2);
  const QModelIndex s11 = model->index(0, 0, ch1);
  EXPECT_TRUE(outline().tree()->isExpanded(s11));  // 절도 /Count 1
  EXPECT_EQ(model->index(0, 0, s11).data().toString(), QStringLiteral("1.1.1 항 (page 3)"));
  // 첫 쪽에서는 첫 장이 현재 항목
  ASSERT_TRUE(spin_until([&] { return outline().highlighted_item() >= 0; }, 3000));
  EXPECT_EQ(highlighted_title(), "제 1 장 (page 1)");

  if (const char* shot = std::getenv("ONPDF_QA_SHOT")) {
    spin_until([&] { return window_.document_view()->pending_visible_tiles() == 0; }, 3000);
    window_.grab().save(QString::fromLocal8Bit(shot));
  }
}

TEST_F(SidebarTest, ActivatingItemNavigatesAndHighlights) {
  ASSERT_TRUE(open("bookmarks.pdf", 15));
  const int item = find_item("2.1 절 (page 5)");
  ASSERT_GE(item, 0);
  const auto& target = outline().outline()->items[static_cast<std::size_t>(item)].target;
  ASSERT_TRUE(target && target->user_location);
  outline().activate_item(item);
  // /XYZ 위치는 워커에서 쪽 포인트로 바꾼 뒤 이동한다
  ASSERT_TRUE(spin_until([&] { return window_.document_view()->current_page() == target->page; }));
  // 대상 쪽 맨 위가 뷰포트 상단(여백 아래)에 오고, 읽는 위치가 그 지점이다
  const QPointF top = window_.document_view()->page_point_to_viewport(target->page, {0, 0});
  EXPECT_GE(top.y(), 0.0);
  EXPECT_LE(top.y(), 20.0);
  auto pos = window_.document_view()->reading_position();
  ASSERT_TRUE(pos);
  EXPECT_EQ(pos->first, target->page);
  ASSERT_TRUE(spin_until([&] { return highlighted_title() == "2.1 절 (page 5)"; }, 3000))
      << highlighted_title();
}

TEST_F(SidebarTest, ScrollingMovesOutlineHighlight) {
  ASSERT_TRUE(open("bookmarks.pdf", 15));
  window_.document_view()->go_to_page(7);
  ASSERT_TRUE(spin_until([&] { return highlighted_title() == "3.1 절 (page 8)"; }, 3000)) << highlighted_title();
  window_.document_view()->go_to_page(0);
  ASSERT_TRUE(spin_until([&] { return highlighted_title() == "제 1 장 (page 1)"; }, 3000)) << highlighted_title();
}

TEST_F(SidebarTest, DocumentWithoutOutlineKeepsThumbnailTab) {
  ASSERT_TRUE(open("simple.pdf", 0));
  settle(100);  // 빈 목차 응답 처리
  EXPECT_EQ(window_.sidebar()->currentWidget(), window_.thumbnail_panel());
  EXPECT_EQ(outline().row_count(), 0);
  EXPECT_EQ(thumbs().list()->model()->rowCount(), 2);
}

TEST_F(SidebarTest, ThumbnailClickNavigatesAndFollowsView) {
  ASSERT_TRUE(open("simple.pdf", 0));
  ASSERT_TRUE(spin_until([&] { return thumbs().cached_count() == 2 && thumbs().pending_requests() == 0; }));
  EXPECT_FALSE(thumbs().thumbnail(0).isNull());
  // 가로 쪽 썸네일은 폭보다 낮다
  EXPECT_LT(thumbs().thumb_size(1).height(), thumbs().thumb_size(1).width());
  EXPECT_GT(thumbs().thumb_size(0).height(), thumbs().thumb_size(0).width());

  emit thumbs().list()->clicked(thumbs().list()->model()->index(1, 0));
  ASSERT_TRUE(spin_until([&] { return window_.document_view()->current_page() == 1; }));
  EXPECT_EQ(thumbs().current_page(), 1);

  // 키보드 ↑ 로 이전 쪽
  thumbs().list()->setFocus();
  QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
  QApplication::sendEvent(thumbs().list(), &up);
  ASSERT_TRUE(spin_until([&] { return window_.document_view()->current_page() == 0; }));
  // 포커스가 목록에 와도(현재 행 자동 지정) 쪽이 바뀌지 않는다
  window_.document_view()->go_to_page(1);
  ASSERT_TRUE(spin_until([&] { return thumbs().current_page() == 1; }));
  window_.document_view()->setFocus();
  thumbs().list()->setFocus();
  settle(50);
  EXPECT_EQ(window_.document_view()->current_page(), 1);

  window_.document_view()->go_to_page(0);
  ASSERT_TRUE(spin_until([&] { return thumbs().current_page() == 0; }));
  EXPECT_EQ(thumbs().list()->currentIndex().row(), 0);
  EXPECT_EQ(window_.document_view()->current_page(), 0);  // 강조 동기화가 이동을 일으키지 않는다
}

TEST_F(SidebarTest, LargeDocumentRequestsOnlyNearVisibleRows) {
  ASSERT_TRUE(open("large-1000.pdf", 0));
  ASSERT_EQ(thumbs().list()->model()->rowCount(), 1000);
  ASSERT_TRUE(spin_until([&] { return thumbs().cached_count() > 0 && thumbs().pending_requests() == 0; }));
  const int initial = thumbs().requested_total();
  EXPECT_LE(initial, 20);  // 보이는 행 + 앞뒤 4

  // 끝까지 빠르게 스크롤: 지나간 쪽은 요청하지 않거나 취소된다
  QScrollBar* bar = thumbs().list()->verticalScrollBar();
  int max_pending = 0;
  for (int step = 1; step <= 20; ++step) {
    bar->setValue(bar->maximum() * step / 20);
    QApplication::processEvents();
    max_pending = std::max(max_pending, thumbs().pending_requests());
  }
  EXPECT_LE(max_pending, ThumbnailPanel::kMaxConcurrent);
  ASSERT_TRUE(spin_until([&] { return thumbs().pending_requests() == 0 && !thumbs().thumbnail(999).isNull(); }));
  // 1,000쪽 전부가 아니라 지나간 화면 근처만 요청했다
  EXPECT_LT(thumbs().requested_total(), 250);
  RecordProperty("thumbnail_requests", thumbs().requested_total());
  if (const char* shot = std::getenv("ONPDF_QA_SHOT_THUMBS")) {
    window_.grab().save(QString::fromLocal8Bit(shot));
  }
  EXPECT_TRUE(thumbs().thumbnail(500).isNull() || thumbs().requested_total() > initial);
}

TEST_F(SidebarTest, F4TogglesSidebar) {
  ASSERT_TRUE(open("simple.pdf", 0));
  QAction* action = window_.sidebar_action();
  ASSERT_NE(action, nullptr);
  EXPECT_EQ(action->shortcut(), QKeySequence(Qt::Key_F4));
  EXPECT_TRUE(action->isChecked());
  action->trigger();
  EXPECT_FALSE(window_.sidebar()->isVisible());
  EXPECT_FALSE(action->isChecked());
  action->trigger();
  EXPECT_TRUE(window_.sidebar()->isVisible());
}

TEST_F(SidebarTest, ReopenDiscardsStaleResults) {
  // 1,000쪽 문서의 썸네일·목차 요청이 진행 중일 때 다른 문서를 연다
  window_.open_file(QString::fromStdString(onpdf::path_to_utf8(testdata("large-1000.pdf"))));
  ASSERT_TRUE(spin_until([&] { return thumbs().list()->model()->rowCount() == 1000; }));
  const int before = thumbs().generation();
  ASSERT_TRUE(open("bookmarks.pdf", 15));
  EXPECT_GT(thumbs().generation(), before);
  EXPECT_EQ(thumbs().list()->model()->rowCount(), 10);
  ASSERT_TRUE(spin_until([&] { return thumbs().pending_requests() == 0; }));
  settle(100);
  EXPECT_LE(thumbs().cached_count(), 10);
  EXPECT_EQ(outline().row_count(), 15);
}

TEST_F(SidebarTest, MaliciousOutlineIsBounded) {
  ASSERT_TRUE(open("outline-cycle.pdf", 33));
  const auto* o = outline().outline();
  ASSERT_NE(o, nullptr);
  EXPECT_TRUE(o->cycle_detected);
  EXPECT_TRUE(o->truncated);
  EXPECT_EQ(outline().tree()->model()->rowCount(), 2);
}

// 쪽 번호 상자에 입력한 번호가 짧은 쪽·마지막 쪽에서도 그대로 남는다 (#12)
TEST_F(SidebarTest, PageSpinKeepsTypedPage) {
  ASSERT_TRUE(open("mixed-sizes.pdf", 0));
  auto* spin = window_.findChild<QSpinBox*>();
  ASSERT_NE(spin, nullptr);
  spin->setValue(5);  // 명함 쪽
  QApplication::processEvents();
  EXPECT_EQ(window_.document_view()->current_page(), 4);
  EXPECT_EQ(spin->value(), 5);
  EXPECT_EQ(thumbs().current_page(), 4);
  spin->setValue(spin->maximum());
  QApplication::processEvents();
  EXPECT_EQ(spin->value(), spin->maximum());
  EXPECT_EQ(window_.document_view()->current_page(), spin->maximum() - 1);
}

// 내부 링크 클릭: 위치가 추출 때 변환되어 있으므로 워커를 거치지 않고 클릭 즉시 그 지점으로 이동한다 (#12)
TEST_F(SidebarTest, InternalLinkNavigatesImmediately) {
  ASSERT_TRUE(open("annotations.pdf", 0));
  auto* view = window_.document_view();
  ASSERT_TRUE(spin_until([&] { return view->text_index(0) != nullptr; }));
  const view::Link* link = nullptr;
  for (const auto& l : view->text_index(0)->links) {
    if (l.kind == view::LinkKind::kPage) link = &l;
  }
  ASSERT_NE(link, nullptr);
  ASSERT_TRUE(link->location.has_value());  // 링크는 즉시 변환
  const QPointF at = view->page_point_to_viewport(0, {link->box.x + link->box.w / 2, link->box.y + link->box.h / 2});
  for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
    QMouseEvent ev(type, at, view->viewport()->mapToGlobal(at.toPoint()), Qt::LeftButton,
                   type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &ev);
  }
  // 이벤트 루프를 돌리지 않아도 이미 이동했다 (워커 왕복 없음)
  EXPECT_EQ(view->current_page(), 1);
  // /XYZ 72 700 → 2쪽 포인트 (72, 92) 가 보인다 (마지막 쪽이라 맨 위까지는 못 올라갈 수 있다)
  const QPointF target = view->page_point_to_viewport(1, {72, 92});
  EXPECT_GE(target.y(), 0.0);
  EXPECT_LE(target.y(), view->viewport()->height());
}

// 썸네일 스크롤바를 끄는 동안에는 요청하지 않고, 놓으면 그 자리를 요청한다 (#12)
TEST_F(SidebarTest, ThumbnailSliderDragDefersRequests) {
  ASSERT_TRUE(open("large-1000.pdf", 0));
  ASSERT_TRUE(spin_until([&] { return thumbs().cached_count() > 0 && thumbs().pending_requests() == 0; }));
  QScrollBar* bar = thumbs().list()->verticalScrollBar();
  const int before = thumbs().requested_total();
  bar->setSliderDown(true);
  for (int step = 1; step <= 10; ++step) {
    bar->setValue(bar->maximum() * step / 20);
    QApplication::processEvents();
  }
  settle(100);
  EXPECT_EQ(thumbs().requested_total(), before) << "no requests while dragging";
  bar->setSliderDown(false);  // sliderReleased
  ASSERT_TRUE(spin_until([&] { return thumbs().requested_total() > before && thumbs().pending_requests() == 0; }));
  const QModelIndex mid = thumbs().list()->indexAt(thumbs().list()->viewport()->rect().center());
  ASSERT_TRUE(mid.isValid());
  EXPECT_FALSE(thumbs().thumbnail(mid.row()).isNull());
  EXPECT_LE(thumbs().requested_total() - before, 20);  // 놓은 자리 근처만
}

// 화면 배율이 바뀌면(모니터 이동) 썸네일을 새 배율로 다시 렌더한다 (#12)
TEST_F(SidebarTest, DevicePixelRatioChangeRerendersThumbnails) {
  ASSERT_TRUE(open("simple.pdf", 0));
  ASSERT_TRUE(spin_until([&] { return thumbs().cached_count() == 2 && thumbs().pending_requests() == 0; }));
  const int generation = thumbs().generation();
  const int requested = thumbs().requested_total();
  QEvent change(QEvent::DevicePixelRatioChange);
  QApplication::sendEvent(&thumbs(), &change);
  EXPECT_GT(thumbs().generation(), generation);
  EXPECT_FALSE(thumbs().thumbnail(0).isNull());  // 새 그림이 올 때까지 이전 그림을 늘려 그린다
  ASSERT_TRUE(spin_until([&] { return thumbs().requested_total() >= requested + 2 && thumbs().pending_requests() == 0; }));
  EXPECT_EQ(thumbs().cached_count(), 2);
}

// 연속(관성) 스크롤 중에도 목차 강조가 따라간다: 강조 타이머는 스크롤마다 다시 시작하지 않는다(스로틀).
// 디바운스면 스크롤이 계속되는 동안 타이머가 계속 미뤄져 강조가 멈춘다 (#12)
TEST_F(SidebarTest, OutlineHighlightTimerIsThrottledNotDebounced) {
  ASSERT_TRUE(open("bookmarks.pdf", 15));
  auto* timer = window_.findChild<QTimer*>(QStringLiteral("outline_highlight_timer"));
  ASSERT_NE(timer, nullptr);
  settle(100);
  ASSERT_FALSE(timer->isActive());
  QScrollBar* bar = window_.document_view()->verticalScrollBar();
  bar->setValue(bar->value() + 30);  // 스크롤 → 타이머 시작
  ASSERT_TRUE(timer->isActive());
  const int first = timer->remainingTime();
  // 이벤트를 처리하지 않고 15ms 기다린 뒤(타이머가 만료될 수 없음) 다시 스크롤
  QElapsedTimer wait;
  wait.start();
  while (wait.elapsed() < 15) {
  }
  bar->setValue(bar->value() + 30);
  ASSERT_TRUE(timer->isActive());
  EXPECT_LE(timer->remainingTime(), first - 10) << "timer was restarted (debounce) instead of kept (throttle)";
  EXPECT_GT(first, 80);  // 100ms 간격
}

// 다른 문서를 열면 이전 문서의 목차 로드 작업을 취소한다 (#12)
TEST_F(SidebarTest, ReopenCancelsPreviousOutlineLoad) {
  ASSERT_TRUE(open("bookmarks.pdf", 15));
  const engine::TaskHandle previous = window_.outline_task();
  ASSERT_TRUE(previous.valid());
  EXPECT_FALSE(previous.is_cancelled());
  ASSERT_TRUE(open("outline-cycle.pdf", 33));
  EXPECT_TRUE(previous.is_cancelled());
  EXPECT_FALSE(window_.outline_task().is_cancelled());
}

TEST_F(SidebarTest, OutlineIndentationFitsNarrowSidebar) {
  ASSERT_TRUE(open("outline-cycle.pdf", 33));
  EXPECT_EQ(outline().tree()->indentation(), 16);
}

// 쪽이 바뀌지 않아도 타일이 도착하면 상태 표시줄 통계를 갱신한다 — 1초에 한 번으로 묶는다 (#12)
TEST_F(SidebarTest, StatusBarRefreshesWhenTilesArrive) {
  ASSERT_TRUE(open("simple.pdf", 0));
  auto* view = window_.document_view();
  auto* timer = window_.findChild<QTimer*>(QStringLiteral("status_timer"));
  ASSERT_NE(timer, nullptr);
  ASSERT_TRUE(spin_until([&] { return view->pending_visible_tiles() == 0; }));
  ASSERT_TRUE(spin_until([&] { return !timer->isActive(); }, 2000));
  QLabel* status = nullptr;
  for (auto* label : window_.statusBar()->findChildren<QLabel*>()) {
    if (label->text().contains(QStringLiteral("MB"))) status = label;
  }
  ASSERT_NE(status, nullptr);
  status->setText(QStringLiteral("stale"));  // 갱신 여부를 보기 위한 표지
  emit view->tiles_changed();
  ASSERT_TRUE(timer->isActive());
  const int first = timer->remainingTime();
  QElapsedTimer wait;
  wait.start();
  while (wait.elapsed() < 20) {
  }
  emit view->tiles_changed();  // 연속 도착: 다시 시작하지 않는다
  EXPECT_LE(timer->remainingTime(), first - 15);
  EXPECT_EQ(status->text(), QStringLiteral("stale"));  // 아직 갱신 전
  EXPECT_GT(first, 900);  // 1초 간격
  ASSERT_TRUE(spin_until([&] { return status->text().contains(QStringLiteral("MB")); }, 3000));
}

// 실제 화면 이동: 창을 배율이 다른 화면으로 옮기면 썸네일이 그 배율로 다시 렌더된다 (#12).
// 배율이 다른 화면이 둘 이상 있을 때만 (예: macOS 내장 Retina + 외장 모니터). offscreen·단일 화면이면 건너뛴다
TEST_F(SidebarTest, MovingToScreenWithDifferentDprRerendersThumbnails) {
  ASSERT_TRUE(open("simple.pdf", 0));
  QScreen* home = window_.screen();
  QScreen* other = nullptr;
  for (QScreen* screen : QGuiApplication::screens()) {
    if (screen->devicePixelRatio() != home->devicePixelRatio()) other = screen;
  }
  if (other == nullptr) GTEST_SKIP() << "needs two screens with different device pixel ratios";
  ASSERT_TRUE(spin_until([&] { return thumbs().cached_count() == 2 && thumbs().pending_requests() == 0; }));

  auto rendered_at = [&](qreal dpr) {
    const QImage image = thumbs().thumbnail(0);
    return !image.isNull() && image.devicePixelRatio() == dpr && thumbs().pending_requests() == 0 &&
           std::abs(image.width() - static_cast<int>(thumbs().thumb_width() * dpr)) <= 2;
  };
  ASSERT_TRUE(rendered_at(home->devicePixelRatio()));
  for (QScreen* target : {other, home}) {
    window_.move(target->availableGeometry().topLeft() + QPoint(40, 40));
    ASSERT_TRUE(spin_until([&] { return thumbs().devicePixelRatioF() == target->devicePixelRatio(); }, 5000))
        << "window did not move to screen " << target->name().toStdString();
    EXPECT_TRUE(spin_until([&] { return rendered_at(target->devicePixelRatio()); }, 5000))
        << "thumbnails not re-rendered at dpr " << target->devicePixelRatio();
  }
}

}  // namespace
}  // namespace onpdf::app
