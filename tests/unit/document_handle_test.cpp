#include "onpdf/core/engine/document_handle.h"

#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/pdfium_worker.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <latch>
#include <memory>
#include <stdexcept>
#include <thread>

namespace onpdf::engine {
namespace {

using namespace std::chrono_literals;
using onpdf::test::expect_bitmap_matches_png;
using onpdf::test::testdata;
constexpr auto kTimeout = 5s;

template <class T>
T get_or_fail(std::future<T>& f) {
  if (f.wait_for(kTimeout) != std::future_status::ready) {
    ADD_FAILURE() << "future timed out";
    throw std::runtime_error("future timed out");
  }
  return f.get();
}

class DocumentHandleTest : public ::testing::Test {
 protected:
  DocumentHandleTest() : worker_(make_options()) {}

  static PdfiumWorker::Options make_options() {
    PdfiumWorker::Options o;
    o.use_system_fonts = false;  // 기준 PNG 와 같은 폰트 설정
    return o;
  }

  DocumentHandle open_simple() {
    auto f = DocumentHandle::open(worker_, testdata("simple.pdf"));
    auto r = get_or_fail(f);
    EXPECT_TRUE(r) << (r ? "" : r.error().message);
    return r ? *r : DocumentHandle{};
  }

  bool is_registered(DocumentId id) {
    auto f = worker_.submit([&, id] { return worker_.find_document(id) != nullptr; });
    auto r = get_or_fail(f);
    return r && *r;
  }

  PdfiumWorker worker_;
};

TEST_F(DocumentHandleTest, EmptyHandleIsInvalid) {
  DocumentHandle empty;
  EXPECT_FALSE(empty.valid());
  EXPECT_EQ(empty.id(), 0u);
  EXPECT_EQ(empty.info().page_count, 0);
  auto f = empty.render_page(0, RenderOptions{});
  auto r = get_or_fail(f);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, ErrorCode::kInvalidState);
}

TEST_F(DocumentHandleTest, OpenReturnsInfo) {
  DocumentHandle doc = open_simple();
  ASSERT_TRUE(doc.valid());
  EXPECT_NE(doc.id(), 0u);
  EXPECT_EQ(doc.info().page_count, 2);
  ASSERT_EQ(doc.info().page_sizes.size(), 2u);
  EXPECT_DOUBLE_EQ(doc.info().page_sizes[0].width, 612.0);
  EXPECT_DOUBLE_EQ(doc.info().page_sizes[1].width, 792.0);
  EXPECT_TRUE(is_registered(doc.id()));
}

TEST_F(DocumentHandleTest, OpenWithCallbackRunsOnWorkerThread) {
  std::promise<bool> on_worker;
  auto f = on_worker.get_future();
  DocumentHandle received;
  TaskHandle handle = DocumentHandle::open(
      worker_, testdata("simple.pdf"), "", Priority::kHigh, [&](Result<DocumentHandle> r) {
        if (r) received = *r;
        on_worker.set_value(worker_.is_worker_thread());
      });
  EXPECT_TRUE(handle.valid());
  EXPECT_TRUE(get_or_fail(f));
  EXPECT_TRUE(received.valid());
  EXPECT_EQ(received.info().page_count, 2);
}

TEST_F(DocumentHandleTest, OpenWithCallbackAfterStopIsCancelledInline) {
  worker_.stop();
  bool called = false;
  std::thread::id called_on;
  ErrorCode code = ErrorCode::kUnknown;
  DocumentHandle::open(worker_, testdata("simple.pdf"), "", Priority::kHigh,
                       [&](Result<DocumentHandle> r) {
                         called = true;
                         called_on = std::this_thread::get_id();
                         if (!r) code = r.error().code;
                       });
  EXPECT_TRUE(called);
  EXPECT_EQ(called_on, std::this_thread::get_id());
  EXPECT_EQ(code, ErrorCode::kCancelled);
}

TEST_F(DocumentHandleTest, OpenMissingFileFails) {
  auto f = DocumentHandle::open(worker_, testdata("does-not-exist.pdf"));
  auto r = get_or_fail(f);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, ErrorCode::kFileNotFound);
}

TEST_F(DocumentHandleTest, OpenFromMemoryRejectsGarbage) {
  auto f = DocumentHandle::open_from_memory(worker_, {'n', 'o', 'p', 'e'});
  auto r = get_or_fail(f);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, ErrorCode::kInvalidFormat);
}

TEST_F(DocumentHandleTest, RenderMatchesReference) {
  DocumentHandle doc = open_simple();
  ASSERT_TRUE(doc.valid());
  RenderOptions options;
  options.scale = 1.0f;
  auto f = doc.render_page(0, options);
  auto r = get_or_fail(f);
  ASSERT_TRUE(r) << r.error().message;
  expect_bitmap_matches_png(*r, testdata("expected/simple-p0.png"), "worker-simple-p0");
}

TEST_F(DocumentHandleTest, RenderCallbackRunsOnWorkerThread) {
  DocumentHandle doc = open_simple();
  ASSERT_TRUE(doc.valid());
  std::promise<bool> on_worker;
  auto f = on_worker.get_future();
  doc.render_page(1, RenderOptions{}, Priority::kHigh, [&](Result<Bitmap> result) {
    EXPECT_TRUE(result);
    on_worker.set_value(worker_.is_worker_thread());
  });
  EXPECT_TRUE(get_or_fail(f));
}

TEST_F(DocumentHandleTest, RenderOutOfRangePagePropagatesError) {
  DocumentHandle doc = open_simple();
  auto f = doc.render_page(7, RenderOptions{});
  auto r = get_or_fail(f);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, ErrorCode::kPageNotFound);
}

TEST_F(DocumentHandleTest, CloseThenRenderFails) {
  DocumentHandle doc = open_simple();
  const DocumentId id = doc.id();
  doc.close();
  EXPECT_FALSE(doc.valid());
  auto f = doc.render_page(0, RenderOptions{});
  auto r = get_or_fail(f);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, ErrorCode::kInvalidState);
  EXPECT_FALSE(is_registered(id));
}

TEST_F(DocumentHandleTest, LastHandleDestructorUnregisters) {
  DocumentId id = 0;
  {
    DocumentHandle doc = open_simple();
    DocumentHandle copy = doc;
    id = doc.id();
    doc = DocumentHandle{};        // 복사본이 남아 있으므로 아직 등록 상태
    EXPECT_TRUE(is_registered(id));
    EXPECT_TRUE(copy.valid());
  }
  EXPECT_FALSE(is_registered(id));
}

TEST_F(DocumentHandleTest, CancelPendingRender) {
  DocumentHandle doc = open_simple();
  auto entered = std::make_shared<std::latch>(1);
  auto release = std::make_shared<std::latch>(1);
  worker_.post([entered, release](bool) {
    entered->count_down();
    release->wait();
  });
  entered->wait();

  std::promise<Result<Bitmap>> p;
  auto f = p.get_future();
  TaskHandle handle = doc.render_page(0, RenderOptions{}, Priority::kNormal,
                                      [&](Result<Bitmap> r) { p.set_value(std::move(r)); });
  handle.cancel();
  release->count_down();
  auto r = get_or_fail(f);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, ErrorCode::kCancelled);
}

TEST_F(DocumentHandleTest, RenderAfterWorkerStopReturnsCancelled) {
  DocumentHandle doc = open_simple();
  worker_.stop();
  auto f = doc.render_page(0, RenderOptions{});
  auto r = get_or_fail(f);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, ErrorCode::kCancelled);
  doc.close();  // inline cancelled 통지 → 아무것도 안 함
}

TEST(DocumentHandleLifetime, HandleOutlivingWorkerFailsSafely) {
  // 핸들이 워커보다 오래 살아도 UAF 없이 kInvalidState 로 끝나야 한다.
  DocumentHandle doc;
  {
    PdfiumWorker worker;
    auto f = DocumentHandle::open(worker, testdata("simple.pdf"));
    auto r = get_or_fail(f);
    ASSERT_TRUE(r) << r.error().message;
    doc = *r;
  }
  EXPECT_TRUE(doc.valid());  // 닫힌 게 아니라 워커가 사라진 것
  bool called = false;
  std::thread::id called_on;
  doc.render_page(0, RenderOptions{}, Priority::kHigh, [&](Result<Bitmap> result) {
    called = true;
    called_on = std::this_thread::get_id();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::kInvalidState);
  });
  EXPECT_TRUE(called);
  EXPECT_EQ(called_on, std::this_thread::get_id());
  doc.close();  // lock 실패 → no-op
  doc = DocumentHandle{};
}

}  // namespace
}  // namespace onpdf::engine
