#include "document_session.h"

#include "onpdf/core/engine/render_types.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "qt_image.h"
#include "ui_dispatcher.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <QEventLoop>
#include <QImage>
#include <QObject>
#include <QString>
#include <QTimer>

#include <vector>

namespace onpdf::app {
namespace {

using onpdf::test::expect_bitmap_matches_png;
using onpdf::test::testdata;

QString testdata_q(const char* name) { return QString::fromStdString(onpdf::path_to_utf8(testdata(name))); }

// 이벤트 루프를 돌리며 predicate 가 참이 될 때까지 기다린다. 시간 초과면 false.
template <class Pred>
bool spin_until(Pred pred, int timeout_ms = 5000) {
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

class DocumentSessionTest : public ::testing::Test {
 protected:
  DocumentSessionTest() : worker_(make_options()), session_(worker_, dispatcher_) {
    QObject::connect(&session_, &DocumentSession::opened, [&](const QString& p, int n) {
      opened_.push_back({p, n});
    });
    QObject::connect(&session_, &DocumentSession::open_failed,
                     [&](const QString& p, const QString& m) { failed_.push_back({p, m}); });
    QObject::connect(&session_, &DocumentSession::password_required,
                     [&](const QString& p) { password_.push_back(p); });
    QObject::connect(&session_, &DocumentSession::page_rendered,
                     [&](int page, const QImage& img, int ms) { rendered_.push_back({page, img, ms}); });
    QObject::connect(&session_, &DocumentSession::render_failed,
                     [&](int page, const QString& m) { render_failed_.push_back({page, m}); });
  }

  static engine::PdfiumWorker::Options make_options() {
    engine::PdfiumWorker::Options o;
    o.use_system_fonts = false;  // 기준 PNG 와 같은 폰트
    return o;
  }

  bool open_simple() {
    session_.open(testdata_q("simple.pdf"));
    return spin_until([&] { return !opened_.empty() || !failed_.empty(); });
  }

  struct Opened { QString path; int pages; };
  struct Failed { QString path; QString message; };
  struct Rendered { int page; QImage image; int ms; };
  struct RenderFailed { int page; QString message; };

  UiDispatcher dispatcher_;
  engine::PdfiumWorker worker_;
  DocumentSession session_;
  std::vector<Opened> opened_;
  std::vector<Failed> failed_;
  std::vector<QString> password_;
  std::vector<Rendered> rendered_;
  std::vector<RenderFailed> render_failed_;
};

TEST_F(DocumentSessionTest, OpenEmitsOpened) {
  ASSERT_TRUE(open_simple());
  ASSERT_EQ(opened_.size(), 1u);
  EXPECT_EQ(opened_[0].pages, 2);
  EXPECT_TRUE(failed_.empty());
  EXPECT_TRUE(session_.has_document());
  EXPECT_EQ(session_.info().page_sizes.size(), 2u);
  EXPECT_EQ(session_.path(), testdata_q("simple.pdf"));
}

TEST_F(DocumentSessionTest, OpenMissingEmitsFailed) {
  session_.open(testdata_q("does-not-exist.pdf"));
  ASSERT_TRUE(spin_until([&] { return !failed_.empty(); }));
  EXPECT_TRUE(opened_.empty());
  EXPECT_TRUE(failed_[0].message.contains("file not found"));
  EXPECT_FALSE(session_.has_document());
}

TEST_F(DocumentSessionTest, OpenGarbageEmitsFailed) {
  session_.open(testdata_q("not_a_pdf.txt"));
  ASSERT_TRUE(spin_until([&] { return !failed_.empty(); }));
  EXPECT_TRUE(failed_[0].message.contains("invalid PDF format"));
}

TEST_F(DocumentSessionTest, RenderEmitsImageMatchingReference) {
  ASSERT_TRUE(open_simple());
  session_.render_page(0, 1.0f, 1.0);
  ASSERT_TRUE(spin_until([&] { return !rendered_.empty() || !render_failed_.empty(); }));
  ASSERT_EQ(rendered_.size(), 1u);
  EXPECT_EQ(rendered_[0].page, 0);
  EXPECT_GE(rendered_[0].ms, 0);
  Bitmap actual = to_bitmap(rendered_[0].image);
  expect_bitmap_matches_png(actual, testdata("expected/simple-p0.png"), "session-simple-p0");
}

TEST_F(DocumentSessionTest, RenderAppliesDevicePixelRatio) {
  ASSERT_TRUE(open_simple());
  session_.render_page(1, 0.5f, 2.0);
  ASSERT_TRUE(spin_until([&] { return !rendered_.empty(); }));
  EXPECT_EQ(rendered_[0].image.width(), 396);  // 792 * 0.5
  EXPECT_DOUBLE_EQ(rendered_[0].image.devicePixelRatio(), 2.0);
}

TEST_F(DocumentSessionTest, NewRenderCancelsPrevious) {
  ASSERT_TRUE(open_simple());
  session_.render_page(0, 1.0f, 1.0);
  session_.render_page(1, 0.5f, 1.0);  // 첫 요청은 취소되거나, 완료돼도 세대 불일치로 무시
  ASSERT_TRUE(spin_until([&] { return !rendered_.empty(); }));
  // 잠깐 더 돌려 늦은 결과가 없는지 확인
  spin_until([] { return false; }, 200);
  ASSERT_EQ(rendered_.size(), 1u);
  EXPECT_EQ(rendered_[0].page, 1);
  EXPECT_TRUE(render_failed_.empty());
}

TEST_F(DocumentSessionTest, StaleOpenIgnored) {
  session_.open(testdata_q("simple.pdf"));
  session_.open(testdata_q("does-not-exist.pdf"));
  ASSERT_TRUE(spin_until([&] { return !failed_.empty(); }));
  spin_until([] { return false; }, 200);
  EXPECT_TRUE(opened_.empty());  // 첫 요청 결과는 무시
  EXPECT_FALSE(session_.has_document());
}

TEST_F(DocumentSessionTest, CloseThenRenderEmitsNothing) {
  ASSERT_TRUE(open_simple());
  session_.close();
  EXPECT_FALSE(session_.has_document());
  session_.render_page(0, 1.0f, 1.0);
  spin_until([] { return false; }, 200);
  EXPECT_TRUE(rendered_.empty());
  EXPECT_TRUE(render_failed_.empty());
}

TEST_F(DocumentSessionTest, RenderInvalidPageEmitsRenderFailed) {
  ASSERT_TRUE(open_simple());
  session_.render_page(9, 1.0f, 1.0);
  ASSERT_TRUE(spin_until([&] { return !render_failed_.empty(); }));
  EXPECT_EQ(render_failed_[0].page, 9);
  EXPECT_TRUE(render_failed_[0].message.contains("page not found"));
}

TEST(UiDispatcher, DropsCallbackAfterReceiverDeleted) {
  UiDispatcher dispatcher;
  auto* receiver = new QObject();
  bool called = false;
  dispatcher.post_to(receiver, [&](QObject*) { called = true; });
  delete receiver;
  spin_until([] { return false; }, 100);
  EXPECT_FALSE(called);

  QObject alive;
  dispatcher.post_to(&alive, [&](QObject*) { called = true; });
  ASSERT_TRUE(spin_until([&] { return called; }));
}

}  // namespace
}  // namespace onpdf::app
