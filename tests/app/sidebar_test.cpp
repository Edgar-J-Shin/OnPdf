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
#include <QEventLoop>
#include <QKeyEvent>
#include <QListView>
#include <QScrollBar>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QTreeView>

#include <algorithm>
#include <cstdlib>
#include <string>

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

}  // namespace
}  // namespace onpdf::app
