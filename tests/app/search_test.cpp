#include "document_view.h"
#include "main_window.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/platform/path.h"
#include "search_bar.h"
#include "ui_dispatcher.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QEventLoop>
#include <QKeyEvent>
#include <QLineEdit>
#include <QScrollBar>
#include <QTimer>

#include <chrono>
#include <cstdlib>

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

void press(QWidget* w, int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
  QKeyEvent down(QEvent::KeyPress, key, mods);
  QApplication::sendEvent(w, &down);
}

TEST(SearchBar, DebouncesAndEmitsKeys) {
  SearchBar bar;
  int queries = 0, next = 0, prev = 0, closed = 0;
  QString last;
  QObject::connect(&bar, &SearchBar::query_changed, &bar, [&](const QString& q, view::SearchOptions) {
    ++queries;
    last = q;
  });
  QObject::connect(&bar, &SearchBar::next_requested, &bar, [&] { ++next; });
  QObject::connect(&bar, &SearchBar::previous_requested, &bar, [&] { ++prev; });
  QObject::connect(&bar, &SearchBar::closed, &bar, [&] { ++closed; });
  bar.open_with({});
  EXPECT_TRUE(bar.isVisible());
  bar.line_edit()->setText(QStringLiteral("p"));
  bar.line_edit()->setText(QStringLiteral("pa"));
  bar.line_edit()->setText(QStringLiteral("page"));
  EXPECT_EQ(queries, 0);  // 디바운스 중
  ASSERT_TRUE(spin_until([&] { return queries > 0; }, 2000));
  EXPECT_EQ(queries, 1);
  EXPECT_EQ(last, QStringLiteral("page"));
  press(bar.line_edit(), Qt::Key_Return);
  press(bar.line_edit(), Qt::Key_Return, Qt::ShiftModifier);
  EXPECT_EQ(next, 1);
  EXPECT_EQ(prev, 1);
  // 디바운스 대기 중 Enter 는 먼저 검색을 확정한다
  bar.line_edit()->setText(QStringLiteral("test"));
  press(bar.line_edit(), Qt::Key_Return);
  EXPECT_EQ(queries, 2);
  EXPECT_EQ(last, QStringLiteral("test"));
  press(bar.line_edit(), Qt::Key_Escape);
  EXPECT_EQ(closed, 1);
  EXPECT_FALSE(bar.isVisible());
}

class SearchFlowTest : public ::testing::Test {
 protected:
  SearchFlowTest() : worker_(options()), window_(worker_, dispatcher_) {
    window_.resize(900, 900);
    window_.show();
  }
  static engine::PdfiumWorker::Options options() {
    engine::PdfiumWorker::Options o;
    o.use_system_fonts = false;
    return o;
  }
  bool open(const char* name) {
    window_.open_file(QString::fromStdString(onpdf::path_to_utf8(testdata(name))));
    return spin_until([&] { return window_.document_view()->has_document() && window_.search_session() != nullptr; });
  }
  bool search(const QString& q) {
    window_.open_find();
    window_.search_bar()->line_edit()->setText(q);
    press(window_.search_bar()->line_edit(), Qt::Key_Return);  // 디바운스 생략 + 첫 next
    return spin_until([&] { return !window_.search_session()->searching(); });
  }
  const view::SearchSession& s() const { return *window_.search_session(); }

  UiDispatcher dispatcher_;
  engine::PdfiumWorker worker_;
  MainWindow window_;
};

TEST_F(SearchFlowTest, FindsNavigatesAndWraps) {
  ASSERT_TRUE(open("simple.pdf"));
  ASSERT_TRUE(search(QStringLiteral("page")));
  EXPECT_EQ(s().total(), 2u);
  ASSERT_TRUE(s().current().has_value());
  // 입력 직후 Enter 는 검색을 시작할 뿐(결과는 비동기) — 첫 결과가 자동으로 현재가 된다
  EXPECT_EQ(s().current_ordinal(), 1u);
  EXPECT_EQ(s().current()->page, 0);
  EXPECT_EQ(window_.search_bar()->status_text(), QStringLiteral("1 / 2"));
  press(window_.search_bar()->line_edit(), Qt::Key_Return);  // 다음
  EXPECT_EQ(s().current_ordinal(), 2u);
  EXPECT_EQ(s().current()->page, 1);
  EXPECT_EQ(window_.search_bar()->status_text(), QStringLiteral("2 / 2"));
  EXPECT_EQ(window_.document_view()->current_page(), 1);  // 결과 쪽으로 스크롤
  press(window_.search_bar()->line_edit(), Qt::Key_Return);  // 끝에서 순환
  EXPECT_EQ(s().current_ordinal(), 1u);
  EXPECT_TRUE(s().wrapped());
  press(window_.search_bar()->line_edit(), Qt::Key_Return, Qt::ShiftModifier);  // 처음에서 끝으로 순환
  EXPECT_EQ(s().current_ordinal(), 2u);
  EXPECT_TRUE(s().wrapped());
  // QA 용: ONPDF_QA_SHOT 이 있으면 하이라이트 상태 창을 PNG 로 저장
  if (const char* shot = std::getenv("ONPDF_QA_SHOT")) {
    window_.document_view()->set_zoom(0.6);
    spin_until([&] { return window_.document_view()->pending_visible_tiles() == 0; }, 3000);
    window_.grab().save(QString::fromLocal8Bit(shot));
  }
  // 하이라이트에 필요한 인덱스가 뷰 캐시에 들어갔다
  EXPECT_NE(window_.document_view()->text_index(0), nullptr);
  EXPECT_NE(window_.document_view()->text_index(1), nullptr);
  // Esc → 검색 해제
  press(window_.search_bar()->line_edit(), Qt::Key_Escape);
  EXPECT_FALSE(s().active());
  EXPECT_FALSE(window_.search_bar()->isVisible());
}

TEST_F(SearchFlowTest, KoreanAndNoResults) {
  ASSERT_TRUE(open("korean-embedded.pdf"));
  ASSERT_TRUE(search(QStringLiteral("한글")));
  EXPECT_EQ(s().total(), 1u);
  ASSERT_TRUE(s().current().has_value());
  const auto* idx = window_.document_view()->text_index(0);
  ASSERT_NE(idx, nullptr);
  EXPECT_EQ(idx->text(s().current()->range), "한글");
  ASSERT_TRUE(search(QStringLiteral("없는단어")));
  EXPECT_EQ(s().total(), 0u);
  EXPECT_EQ(window_.search_bar()->status_text(), QStringLiteral("결과 없음"));
}

TEST_F(SearchFlowTest, ScannedDocumentReportsNoText) {
  ASSERT_TRUE(open("scanned.pdf"));
  ASSERT_TRUE(search(QStringLiteral("a")));
  EXPECT_TRUE(s().no_text());
  EXPECT_EQ(window_.search_bar()->status_text(), QStringLiteral("텍스트 없음"));
}

TEST_F(SearchFlowTest, LargeDocumentSearchStaysResponsive) {
  ASSERT_TRUE(open("large-1000.pdf"));
  const auto t0 = std::chrono::steady_clock::now();
  window_.open_find();
  window_.search_bar()->line_edit()->setText(QStringLiteral("Page 5"));
  press(window_.search_bar()->line_edit(), Qt::Key_Return);
  // 검색 중에도 뷰가 응답해야 한다: 바로 쪽 이동 가능
  window_.document_view()->go_to_page(700);
  EXPECT_EQ(window_.document_view()->current_page(), 700);
  ASSERT_TRUE(spin_until([&] { return !s().searching(); }, 30000));
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  ::testing::Test::RecordProperty("search_1000_pages_ms", static_cast<int>(ms));
  std::printf("[ perf     ] large-1000 search \"Page 5\": %lld ms\n", static_cast<long long>(ms));
  // "Page 5 /", "Page 5x /", "Page 5xx /" → 1 + 10 + 100
  EXPECT_EQ(s().total(), 111u);
  EXPECT_EQ(s().pages_done(), 1000);
}

TEST_F(SearchFlowTest, ReplacingDocumentRestartsSearch) {
  ASSERT_TRUE(open("simple.pdf"));
  ASSERT_TRUE(search(QStringLiteral("page")));
  EXPECT_EQ(s().total(), 2u);
  // 검색 바가 열린 채 다른 문서를 열면 같은 검색어로 다시 찾는다
  window_.open_file(QString::fromStdString(onpdf::path_to_utf8(testdata("annotations.pdf"))));
  ASSERT_TRUE(spin_until([&] { return window_.document_view()->page_count() == 2 && window_.search_session() &&
                                      window_.search_session()->active() && !window_.search_session()->searching(); }));
  EXPECT_EQ(s().query(), "page");
  EXPECT_GE(s().total(), 1u);  // "go to page 2", "Link target: page 2" 등
}

}  // namespace
}  // namespace onpdf::app
