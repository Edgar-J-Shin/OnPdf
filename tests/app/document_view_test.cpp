#include "document_view.h"

#include "document_session.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/platform/path.h"
#include "ui_dispatcher.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <QEventLoop>
#include <QScrollBar>
#include <QTimer>

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
  loop.exec();
  return !timed_out;
}

class DocumentViewTest : public ::testing::Test {
 protected:
  DocumentViewTest() : worker_(options()), session_(worker_, dispatcher_) {
    view_.resize(800, 600);
    view_.show();  // offscreen 이지만 레이아웃·뷰포트 크기 확정에 필요
  }
  static engine::PdfiumWorker::Options options() {
    engine::PdfiumWorker::Options o;
    o.use_system_fonts = false;
    return o;
  }
  bool open(const char* name) {
    bool opened = false, failed = false;
    QObject::connect(&session_, &DocumentSession::opened, &view_, [&](const QString&, int) { opened = true; });
    QObject::connect(&session_, &DocumentSession::open_failed, &view_, [&](const QString&, const QString&) { failed = true; });
    session_.open(QString::fromStdString(onpdf::path_to_utf8(testdata(name))));
    if (!spin_until([&] { return opened || failed; }) || failed) return false;
    view_.set_document(session_.handle(), dispatcher_);
    return true;
  }
  bool wait_first_view() {
    bool done = false;
    QObject::connect(&view_, &DocumentView::first_view_rendered, &view_, [&] { done = true; });
    if (view_.pending_visible_tiles() == 0) return true;
    return spin_until([&] { return done; });
  }

  UiDispatcher dispatcher_;
  engine::PdfiumWorker worker_;
  DocumentSession session_;
  DocumentView view_;
};

TEST_F(DocumentViewTest, OpensAndRendersFirstView) {
  ASSERT_TRUE(open("simple.pdf"));
  EXPECT_EQ(view_.page_count(), 2);
  EXPECT_EQ(view_.current_page(), 0);
  EXPECT_EQ(view_.zoom_mode(), ZoomMode::kFitWidth);
  ASSERT_TRUE(wait_first_view());
  EXPECT_EQ(view_.pending_visible_tiles(), 0);
  EXPECT_GT(view_.cache_stats().count, 0u);
  EXPECT_GT(view_.scheduler_stats().completed, 0);
}

TEST_F(DocumentViewTest, ScrollingLargeDocumentRequestsBoundedTiles) {
  ASSERT_TRUE(open("large-1000.pdf"));
  ASSERT_EQ(view_.page_count(), 1000);
  QScrollBar* v = view_.verticalScrollBar();
  const int max = v->maximum();
  ASSERT_GT(max, 0);
  // 10단계로 끝까지 이동. 각 단계에서 요청은 뷰포트 타일 + 미리보기 + 앞뒤 쪽 프리페치 정도여야 한다
  for (int i = 1; i <= 10; ++i) v->setValue(max * i / 10);
  EXPECT_EQ(view_.current_page(), 999);
  const int requested = view_.scheduler_stats().requested;
  EXPECT_LT(requested, 10 * 40) << "requests should be bounded by viewport, not document size";
  EXPECT_GT(requested, 10);
  // 지나간 뷰포트의 요청은 취소된다
  EXPECT_GT(view_.scheduler_stats().cancelled, 0);
  ASSERT_TRUE(wait_first_view());
  EXPECT_LE(view_.cache_stats().bytes, view_.cache_stats().count * 512u * 512u * 4u);
}

TEST_F(DocumentViewTest, ZoomKeepsAnchorAndClamps) {
  ASSERT_TRUE(open("mixed-sizes.pdf"));
  view_.set_zoom(1.0);
  EXPECT_EQ(view_.zoom_mode(), ZoomMode::kCustom);
  view_.go_to_page(2);
  const int page_before = view_.current_page();
  const double before_fraction = [&] {
    QScrollBar* v = view_.verticalScrollBar();
    return static_cast<double>(v->value());
  }();
  (void)before_fraction;
  view_.set_zoom(2.0, QPointF(400, 300));
  EXPECT_DOUBLE_EQ(view_.zoom(), 2.0);
  EXPECT_EQ(view_.current_page(), page_before);  // 중앙 앵커 → 같은 쪽이 중앙에 남는다
  view_.set_zoom(100.0);
  EXPECT_DOUBLE_EQ(view_.zoom(), DocumentView::kMaxZoom);
  view_.set_zoom(0.0);
  EXPECT_DOUBLE_EQ(view_.zoom(), DocumentView::kMinZoom);
}

TEST_F(DocumentViewTest, GoToPageAndPageSignal) {
  ASSERT_TRUE(open("bookmarks.pdf"));
  int last = -1;
  QObject::connect(&view_, &DocumentView::current_page_changed, &view_, [&](int p) { last = p; });
  view_.go_to_page(7);
  EXPECT_EQ(view_.current_page(), 7);
  EXPECT_EQ(last, 7);
  view_.go_to_page(999);
  EXPECT_EQ(view_.current_page(), 9);
}

TEST_F(DocumentViewTest, ReplacingDocumentDropsLateTiles) {
  ASSERT_TRUE(open("large-1000.pdf"));
  view_.verticalScrollBar()->setValue(view_.verticalScrollBar()->maximum() / 2);
  const int in_flight_before = view_.scheduler_stats().requested;
  EXPECT_GT(in_flight_before, 0);
  // 즉시 다른 문서로 교체: 이전 문서의 타일이 늦게 도착해도 새 캐시에는 들어가지 않는다
  ASSERT_TRUE(open("simple.pdf"));
  ASSERT_TRUE(wait_first_view());
  for (const auto& [key, bitmap] : std::vector<std::pair<int, int>>{}) (void)key;
  EXPECT_LE(view_.cache_stats().count, 8u);  // simple.pdf 2쪽: 미리보기 2 + 타일 몇 장
  EXPECT_EQ(view_.page_count(), 2);
}

TEST_F(DocumentViewTest, ClearDocumentResetsState) {
  ASSERT_TRUE(open("simple.pdf"));
  ASSERT_TRUE(wait_first_view());
  view_.clear_document(QStringLiteral("empty"));
  EXPECT_FALSE(view_.has_document());
  EXPECT_EQ(view_.current_page(), -1);
  EXPECT_EQ(view_.cache_stats().count, 0u);
  EXPECT_EQ(view_.verticalScrollBar()->maximum(), 0);
}

}  // namespace
}  // namespace onpdf::app
