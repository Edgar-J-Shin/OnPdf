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

#include <cmath>

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
  // 람다가 지역 변수를 참조로 잡으므로 반환 전에 반드시 연결을 끊는다 (두 번째 open 에서 죽은 스택 접근 방지)
  bool open(const char* name) {
    bool opened = false, failed = false;
    const auto c1 = QObject::connect(&session_, &DocumentSession::opened, &view_, [&](const QString&, int) { opened = true; });
    const auto c2 = QObject::connect(&session_, &DocumentSession::open_failed, &view_,
                                     [&](const QString&, const QString&) { failed = true; });
    session_.open(QString::fromStdString(onpdf::path_to_utf8(testdata(name))));
    const bool ok = spin_until([&] { return opened || failed; }) && !failed;
    QObject::disconnect(c1);
    QObject::disconnect(c2);
    if (!ok) return false;
    view_.set_document(session_.handle(), dispatcher_);
    return true;
  }
  bool wait_first_view() {
    if (view_.pending_visible_tiles() == 0) return true;
    bool done = false;
    const auto c = QObject::connect(&view_, &DocumentView::first_view_rendered, &view_, [&] { done = true; });
    const bool ok = spin_until([&] { return done; });
    QObject::disconnect(c);
    return ok;
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
  EXPECT_GT(view_.cache_stats().count, 0u);
  EXPECT_LE(view_.cache_stats().bytes, std::size_t{256} << 20);
}

TEST_F(DocumentViewTest, ZoomKeepsAnchorAndClamps) {
  ASSERT_TRUE(open("mixed-sizes.pdf"));
  view_.set_zoom(1.0);
  EXPECT_EQ(view_.zoom_mode(), ZoomMode::kCustom);
  view_.go_to_page(2);
  const int page_before = view_.current_page();
  view_.set_zoom(2.0, QPointF(400, 300));
  EXPECT_DOUBLE_EQ(view_.zoom(), 2.0);
  EXPECT_EQ(view_.current_page(), page_before);  // 중앙 앵커 → 같은 쪽이 중앙에 남는다
  view_.set_zoom(100.0);
  EXPECT_DOUBLE_EQ(view_.zoom(), DocumentView::kMaxZoom);
  view_.set_zoom(0.0);
  EXPECT_DOUBLE_EQ(view_.zoom(), DocumentView::kMinZoom);
}

TEST_F(DocumentViewTest, HorizontalAnchorWhenDocumentNarrowerThanViewport) {
  // bookmarks.pdf 는 전 쪽 Letter(612pt) 라 100% 에서도 문서 폭(644px)이 뷰포트보다 좁다 → 가운데 정렬 오프셋 존재
  ASSERT_TRUE(open("bookmarks.pdf"));
  view_.set_zoom(0.5);
  const double vw = view_.viewport()->width();
  ASSERT_GT(vw, 700);
  ASSERT_EQ(view_.horizontalScrollBar()->maximum(), 0);
  // 뷰 x = hoff + margin + page_x. 0.5 배율에서 쪽 안 x=150px(= 300pt) 지점을 앵커로 2배 확대
  const double hoff_before = std::floor((vw - (306 + 32)) / 2.0);
  const QPointF anchor(hoff_before + 16 + 150, 100);
  view_.set_zoom(1.0, anchor);
  ASSERT_EQ(view_.horizontalScrollBar()->maximum(), 0);  // 644 < vw → 여전히 스크롤 없음
  // 같은 문서 지점(300pt = 1.0 배율에서 300px) 의 새 뷰 x
  const double hoff_after = std::floor((vw - (612 + 32)) / 2.0);
  const double new_view_x = hoff_after + 16 + 300 - view_.horizontalScrollBar()->value();
  // 가로 스크롤이 불가능하므로 앵커를 정확히 고정할 수는 없지만, 오프셋을 무시한 구현은 수십 px 을 튀게 한다.
  // 올바른 구현은 "스크롤 0 + 새 오프셋" 이라는 유일한 해를 내므로 그 값과 일치해야 한다
  EXPECT_NEAR(new_view_x, hoff_after + 316, 1.0);
  EXPECT_EQ(view_.horizontalScrollBar()->value(), 0);
  EXPECT_EQ(view_.current_page(), 0);
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
  EXPECT_EQ(view_.page_count(), 2);
  // 캐시에는 새 문서(2쪽)의 타일만 있어야 한다. 이전 문서(500쪽 근처)의 늦은 타일이 섞이면 실패
  const auto keys = view_.cache_keys();
  EXPECT_FALSE(keys.empty());
  for (const auto& key : keys) {
    EXPECT_GE(key.page, 0);
    EXPECT_LT(key.page, 2) << "late tile from previous document leaked into cache";
  }
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
