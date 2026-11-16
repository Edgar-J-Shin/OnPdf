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

// 편집은 워커에서 EditableDocument 로 실행되고, 메인 스레드가 apply_change 로 문서 정보를 고친다.
// 저장은 워커에서 안전 저장 절차를 거친다 (#31)
TEST_F(DocumentHandleTest, ModifyApplyChangeAndSave) {
  DocumentHandle doc = open_simple();
  ASSERT_TRUE(doc.valid());
  EXPECT_TRUE(doc.info().permissions.assemble);
  EXPECT_EQ(doc.info().signature_count, 0);
  auto changed_future = doc.modify([](EditableDocument& d) -> Result<ChangeSet> {
    if (auto r = d.set_page_rotation(0, 1); !r) return std::unexpected(r.error());
    ChangeSet change;
    change.pages = {0};
    change.new_sizes.emplace_back(0, *d.page_size(0));
    return change;
  });
  auto changed = get_or_fail(changed_future);
  ASSERT_TRUE(changed) << changed.error().message;
  // 워커는 문서 정보를 건드리지 않는다: 메인 스레드가 apply_change 하기 전에는 그대로
  EXPECT_DOUBLE_EQ(doc.info().page_sizes[0].width, 612.0);
  DocumentHandle copy = doc;  // 같은 문서의 사본에도 보인다
  copy.apply_change(*changed);
  EXPECT_DOUBLE_EQ(doc.info().page_sizes[0].width, 792.0);

  onpdf::test::ScopedTempDir dir;
  const auto target = dir / "saved.pdf";
  auto saved_future = doc.save(target);
  auto saved = get_or_fail(saved_future);
  ASSERT_TRUE(saved) << saved.error().message;
  auto reopened_future = DocumentHandle::open(worker_, target);
  auto reopened = get_or_fail(reopened_future);
  ASSERT_TRUE(reopened) << reopened.error().message;
  EXPECT_DOUBLE_EQ(reopened->info().page_sizes[0].width, 792.0);
}

// 서명 문서는 DocumentHandle::save 가 증분 저장을 강제한다
// 구조 변경(쪽 삭제)이면 apply_change 가 쪽 수와 크기 목록을 통째로 바꾼다
TEST_F(DocumentHandleTest, ModifyStructuralUpdatesPageCount) {
  DocumentHandle doc = open_simple();
  ASSERT_TRUE(doc.valid());
  ASSERT_EQ(doc.info().page_count, 2);
  const PageSize second = doc.info().page_sizes[1];
  auto future = doc.modify([](EditableDocument& d) -> Result<ChangeSet> {
    if (auto r = d.delete_pages({0}); !r) return std::unexpected(r.error());
    ChangeSet change;
    change.structural = true;
    change.all_sizes.push_back(*d.page_size(0));
    return change;
  });
  auto changed = get_or_fail(future);
  ASSERT_TRUE(changed) << changed.error().message;
  EXPECT_EQ(doc.info().page_count, 2);  // apply_change 전
  doc.apply_change(*changed);
  EXPECT_EQ(doc.info().page_count, 1);
  ASSERT_EQ(doc.info().page_sizes.size(), 1u);
  EXPECT_DOUBLE_EQ(doc.info().page_sizes[0].width, second.width);
}

TEST_F(DocumentHandleTest, SignedDocumentSavesIncrementally) {
  auto opened_future = DocumentHandle::open(worker_, testdata("signed-dummy.pdf"));
  auto opened = get_or_fail(opened_future);
  ASSERT_TRUE(opened) << opened.error().message;
  EXPECT_EQ(opened->info().signature_count, 1);
  onpdf::test::ScopedTempDir dir;
  auto saved_future = opened->save(dir / "signed.pdf");  // incremental 을 지정하지 않아도
  auto saved = get_or_fail(saved_future);
  ASSERT_TRUE(saved) << saved.error().message;
  EXPECT_TRUE(saved->incremental);
}

TEST_F(DocumentHandleTest, OpenReturnsPrintPreferences) {
  auto f = DocumentHandle::open(worker_, testdata("print-flags.pdf"));
  auto r = get_or_fail(f);
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_TRUE(r->info().print.allowed);
  EXPECT_TRUE(r->info().print.no_scaling);
  EXPECT_EQ(r->info().print.copies, 2);
  EXPECT_FALSE(open_simple().info().print.no_scaling);

  auto denied_future = DocumentHandle::open(worker_, testdata("encrypted-owner-only.pdf"));
  auto denied = get_or_fail(denied_future);
  ASSERT_TRUE(denied) << denied.error().message;
  EXPECT_FALSE(denied->info().print.allowed);
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
