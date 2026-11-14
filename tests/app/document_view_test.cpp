#include "document_view.h"

#include "document_session.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/platform/path.h"
#include "ui_dispatcher.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QEventLoop>
#include <QKeyEvent>
#include <QMouseEvent>
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

  bool wait_text(int page) {
    if (view_.text_index(page) != nullptr) return true;
    return spin_until([&] { return view_.text_index(page) != nullptr; });
  }
  void send_mouse(QEvent::Type type, QPointF pos, Qt::MouseButton button = Qt::LeftButton,
                  Qt::MouseButtons buttons = Qt::LeftButton) {
    QMouseEvent ev(type, pos, view_.viewport()->mapToGlobal(pos.toPoint()), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(view_.viewport(), &ev);
  }
  void drag(QPointF from, QPointF to) {
    send_mouse(QEvent::MouseButtonPress, from);
    send_mouse(QEvent::MouseMove, (from + to) / 2);
    send_mouse(QEvent::MouseMove, to);
    send_mouse(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
  }
  QPointF char_center(int page, int ch) {
    const auto* index = view_.text_index(page);
    const auto& box = index->chars[static_cast<std::size_t>(ch)].box;
    return view_.page_point_to_viewport(page, {box.x + box.w / 2, box.y + box.h / 2});
  }
  // 드래그 시작점은 글자 왼쪽 가장자리(그 글자부터 포함), 끝점은 오른쪽 가장자리 너머(그 글자까지 포함)
  QPointF char_left(int page, int ch) {
    const auto* index = view_.text_index(page);
    const auto& box = index->chars[static_cast<std::size_t>(ch)].box;
    return view_.page_point_to_viewport(page, {box.x + 0.5, box.y + box.h / 2});
  }
  QPointF char_right(int page, int ch) {
    const auto* index = view_.text_index(page);
    const auto& box = index->chars[static_cast<std::size_t>(ch)].box;
    return view_.page_point_to_viewport(page, {box.right() + 0.5, box.y + box.h / 2});
  }

  UiDispatcher dispatcher_;
  engine::PdfiumWorker worker_;
  DocumentSession session_;
  DocumentView view_;
};

TEST_F(DocumentViewTest, DragSelectsKoreanText) {
  ASSERT_TRUE(open("korean-embedded.pdf"));
  ASSERT_TRUE(wait_text(0));
  const auto* index = view_.text_index(0);
  const view::TextRange line0 = index->lines[0].chars;
  bool changed = false;
  QObject::connect(&view_, &DocumentView::selection_changed, &view_, [&](bool has) { changed = has; });
  drag(char_left(0, line0.start), char_right(0, line0.end - 1));
  EXPECT_TRUE(changed);
  EXPECT_TRUE(view_.has_selection());
  EXPECT_EQ(view_.selected_text(), QStringLiteral("안녕하세요, OnPdf 입니다."));
  // 배율이 바뀌어도 같은 글자가 선택된 채
  view_.set_zoom(2.0);
  EXPECT_EQ(view_.selected_text(), QStringLiteral("안녕하세요, OnPdf 입니다."));
}

TEST_F(DocumentViewTest, ReverseDragAndEscape) {
  ASSERT_TRUE(open("korean-embedded.pdf"));
  ASSERT_TRUE(wait_text(0));
  const auto* index = view_.text_index(0);
  const view::TextRange line1 = index->lines[1].chars;
  drag(char_right(0, line1.end - 1), char_left(0, line1.start));
  EXPECT_EQ(view_.selected_text(), QStringLiteral("한글 CID 폰트 임베딩 테스트 문서입니다."));
  QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QApplication::sendEvent(&view_, &esc);
  EXPECT_FALSE(view_.has_selection());
  EXPECT_TRUE(view_.selected_text().isEmpty());
}

TEST_F(DocumentViewTest, DoubleClickSelectsWord) {
  ASSERT_TRUE(open("simple.pdf"));
  ASSERT_TRUE(wait_text(0));
  const auto* index = view_.text_index(0);
  // 줄 0: "OnPdf M0 test page" → 'M0' 의 'M' 은 인덱스 6
  const QPointF at = char_center(0, 6);
  send_mouse(QEvent::MouseButtonPress, at);
  send_mouse(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
  send_mouse(QEvent::MouseButtonDblClick, at);
  send_mouse(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
  EXPECT_EQ(view_.selected_text(), QStringLiteral("M0"));
  (void)index;
}

TEST_F(DocumentViewTest, CrossPageDragJoinsWithBlankLine) {
  ASSERT_TRUE(open("simple.pdf"));
  ASSERT_TRUE(wait_text(0));
  view_.set_zoom(0.4);  // 두 쪽이 한 뷰포트에
  ASSERT_TRUE(wait_text(1));
  const auto* p0 = view_.text_index(0);
  const QPointF from = char_left(0, p0->lines.back().chars.start);
  const QPointF to = char_right(1, 5);
  drag(from, to);
  const QString text = view_.selected_text();
  EXPECT_TRUE(text.startsWith(QStringLiteral("Letter portrait"))) << text.toStdString();
  EXPECT_TRUE(text.contains(QStringLiteral("\n\n"))) << text.toStdString();
  EXPECT_TRUE(text.endsWith(QStringLiteral("Page 2"))) << text.toStdString();
}

TEST_F(DocumentViewTest, ClickOnLinksActivatesWithoutOpening) {
  ASSERT_TRUE(open("annotations.pdf"));
  ASSERT_TRUE(wait_text(0));
  std::vector<view::Link> activated;
  QObject::connect(&view_, &DocumentView::link_activated, &view_, [&](const view::Link& l) { activated.push_back(l); });
  // 내부 링크 Rect [72 360 300 380] → 쪽 포인트 (100, 792-370)
  const QPointF internal = view_.page_point_to_viewport(0, {100, 792 - 370});
  send_mouse(QEvent::MouseButtonPress, internal);
  send_mouse(QEvent::MouseButtonRelease, internal, Qt::LeftButton, Qt::NoButton);
  ASSERT_EQ(activated.size(), 1u);
  EXPECT_EQ(activated[0].kind, view::LinkKind::kPage);
  EXPECT_EQ(activated[0].page, 1);
  // 외부(차단) 링크 Rect [72 320 300 340]
  const QPointF blocked = view_.page_point_to_viewport(0, {100, 792 - 330});
  send_mouse(QEvent::MouseButtonPress, blocked);
  send_mouse(QEvent::MouseButtonRelease, blocked, Qt::LeftButton, Qt::NoButton);
  ASSERT_EQ(activated.size(), 2u);
  EXPECT_EQ(activated[1].kind, view::LinkKind::kUri);
  EXPECT_FALSE(view::is_safe_uri(activated[1].uri));
  // 링크 위에서 끌면 텍스트 선택으로 바뀌고 활성화되지 않는다
  drag(internal, internal + QPointF(60, 0));
  EXPECT_EQ(activated.size(), 2u);
  // 내부 링크 이동
  view_.scroll_to_page_point(activated[0].page, *activated[0].location);
  EXPECT_EQ(view_.current_page(), 1);
}

TEST_F(DocumentViewTest, HoverReportsLinkTarget) {
  ASSERT_TRUE(open("annotations.pdf"));
  ASSERT_TRUE(wait_text(0));
  QString hovered;
  QObject::connect(&view_, &DocumentView::link_hovered, &view_, [&](const QString& t) { hovered = t; });
  send_mouse(QEvent::MouseMove, view_.page_point_to_viewport(0, {100, 792 - 410}), Qt::NoButton, Qt::NoButton);
  EXPECT_EQ(hovered, QStringLiteral("https://github.com/Edgar-J-Shin/OnPdf"));
  EXPECT_EQ(view_.viewport()->cursor().shape(), Qt::PointingHandCursor);
  send_mouse(QEvent::MouseMove, view_.page_point_to_viewport(0, {300, 600}), Qt::NoButton, Qt::NoButton);
  EXPECT_TRUE(hovered.isEmpty());
  EXPECT_EQ(view_.viewport()->cursor().shape(), Qt::ArrowCursor);
}

TEST_F(DocumentViewTest, StaleTextIndexIgnoredAfterReplace) {
  ASSERT_TRUE(open("large-1000.pdf"));
  view_.verticalScrollBar()->setValue(view_.verticalScrollBar()->maximum() / 2);
  ASSERT_TRUE(open("scanned.pdf"));
  ASSERT_TRUE(wait_text(0));
  spin_until([] { return false; }, 150);
  EXPECT_EQ(view_.text_index(0)->char_count(), 0);
  for (int p = 2; p < 1000; p += 97) EXPECT_EQ(view_.text_index(p), nullptr);
  EXPECT_FALSE(view_.has_selection());
}

TEST_F(DocumentViewTest, SelectAllOnCurrentPage) {
  ASSERT_TRUE(open("korean-embedded.pdf"));
  ASSERT_TRUE(wait_text(0));
  view_.select_all_on_current_page();
  EXPECT_TRUE(view_.has_selection());
  EXPECT_EQ(view_.selected_text().split(QLatin1Char('\n')).size(), 4);
  view_.clear_selection();
  EXPECT_FALSE(view_.has_selection());
}


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
