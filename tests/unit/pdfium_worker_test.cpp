#include "onpdf/core/engine/pdfium_worker.h"

#include "onpdf/core/engine/pdfium_library.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <latch>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace onpdf::engine {
namespace {

using namespace std::chrono_literals;
constexpr auto kTimeout = 5s;

// 워커를 첫 작업에서 블록시켜, 그 사이 큐에 쌓인 작업의 순서·취소를 검증한다.
class Gate {
 public:
  explicit Gate(PdfiumWorker& worker) {
    worker.post([this](bool) {
      entered_.count_down();
      release_.wait();
    });
    entered_.wait();  // 워커가 블록 상태에 들어갈 때까지 기다린다
  }
  void open() { release_.count_down(); }

 private:
  std::latch entered_{1};
  std::latch release_{1};
};

template <class T>
T get_or_fail(std::future<T>& f) {
  EXPECT_EQ(f.wait_for(kTimeout), std::future_status::ready) << "future timed out";
  return f.get();
}

TEST(PdfiumWorker, InitializesLibraryOnWorkerThread) {
  PdfiumWorker worker;
  EXPECT_TRUE(PdfiumLibrary::is_initialized());
  EXPECT_FALSE(worker.is_worker_thread());
  EXPECT_FALSE(worker.is_stopped());

  auto f = worker.submit([&] { return worker.is_worker_thread(); });
  auto r = get_or_fail(f);
  ASSERT_TRUE(r);
  EXPECT_TRUE(*r);
}

TEST(PdfiumWorker, DestructorReleasesLibrary) {
  { PdfiumWorker worker; }
  EXPECT_FALSE(PdfiumLibrary::is_initialized());
}

TEST(PdfiumWorker, RunsTasksInFifoOrderWithinPriority) {
  PdfiumWorker worker;
  std::vector<int> order;
  std::mutex m;
  Gate gate(worker);
  std::latch done{5};
  for (int i = 0; i < 5; ++i) {
    worker.post([&, i](bool cancelled) {
      EXPECT_FALSE(cancelled);
      std::lock_guard lock(m);
      order.push_back(i);
      done.count_down();
    });
  }
  EXPECT_EQ(worker.pending_count(), 5u);
  gate.open();
  done.wait();
  EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST(PdfiumWorker, HighPriorityRunsBeforeNormalBeforeLow) {
  PdfiumWorker worker;
  std::vector<std::string> order;
  std::mutex m;
  Gate gate(worker);
  std::latch done{4};
  auto record = [&](std::string name) {
    return [&, name](bool) {
      std::lock_guard lock(m);
      order.push_back(name);
      done.count_down();
    };
  };
  worker.post(record("normal-1"), Priority::kNormal);
  worker.post(record("low"), Priority::kLow);
  worker.post(record("high"), Priority::kHigh);
  worker.post(record("normal-2"), Priority::kNormal);
  gate.open();
  done.wait();
  EXPECT_EQ(order, (std::vector<std::string>{"high", "normal-1", "normal-2", "low"}));
}

TEST(PdfiumWorker, CancelBeforeStartNotifiesWithCancelledFlag) {
  PdfiumWorker worker;
  Gate gate(worker);
  std::promise<bool> seen;
  auto seen_future = seen.get_future();
  TaskHandle handle = worker.post([&](bool cancelled) { seen.set_value(cancelled); });
  ASSERT_TRUE(handle.valid());
  EXPECT_FALSE(handle.is_cancelled());
  handle.cancel();
  EXPECT_TRUE(handle.is_cancelled());
  gate.open();
  EXPECT_TRUE(get_or_fail(seen_future));
}

TEST(PdfiumWorker, SubmitReturnsValue) {
  PdfiumWorker worker;
  auto f = worker.submit([] { return 42; });
  auto r = get_or_fail(f);
  ASSERT_TRUE(r);
  EXPECT_EQ(*r, 42);
}

TEST(PdfiumWorker, SubmitPropagatesResultError) {
  PdfiumWorker worker;
  auto f = worker.submit([]() -> Result<int> {
    return make_error(ErrorCode::kInvalidArgument, "nope");
  });
  auto r = get_or_fail(f);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, ErrorCode::kInvalidArgument);
  EXPECT_EQ(r.error().message, "nope");
}

TEST(PdfiumWorker, SubmitCatchesException) {
  PdfiumWorker worker;
  auto f = worker.submit([]() -> int { throw std::runtime_error("boom"); });
  auto r = get_or_fail(f);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, ErrorCode::kUnknown);
  EXPECT_NE(r.error().message.find("boom"), std::string::npos);

  // 예외 뒤에도 워커는 살아 있다
  auto g = worker.submit([] { return 1; });
  auto r2 = get_or_fail(g);
  ASSERT_TRUE(r2);
}

TEST(PdfiumWorker, SubmitCancelledReturnsCancelledError) {
  PdfiumWorker worker;
  Gate gate(worker);
  auto f = worker.submit([] { return 1; });
  worker.cancel_all();
  gate.open();
  auto r = get_or_fail(f);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, ErrorCode::kCancelled);
}

TEST(PdfiumWorker, CancelAllCancelsEveryPendingTask) {
  PdfiumWorker worker;
  Gate gate(worker);
  std::atomic<int> cancelled_count{0};
  std::latch done{3};
  for (int i = 0; i < 3; ++i) {
    worker.post([&](bool cancelled) {
      if (cancelled) ++cancelled_count;
      done.count_down();
    });
  }
  worker.cancel_all();
  gate.open();
  done.wait();
  EXPECT_EQ(cancelled_count.load(), 3);
}

TEST(PdfiumWorker, DestructorCancelsPendingAndJoins) {
  std::atomic<int> cancelled_count{0};
  std::atomic<int> total{0};
  {
    PdfiumWorker worker;
    Gate gate(worker);
    for (int i = 0; i < 3; ++i) {
      worker.post([&](bool cancelled) {
        ++total;
        if (cancelled) ++cancelled_count;
      });
    }
    gate.open();
    // Gate 가 열리자마자 소멸자가 stop() 을 부르므로, 대기 작업은 취소 통지된다
  }
  EXPECT_EQ(total.load(), 3);
  EXPECT_EQ(cancelled_count.load(), 3);
}

TEST(PdfiumWorker, PostAfterStopRunsCancelledInline) {
  PdfiumWorker worker;
  worker.stop();
  EXPECT_TRUE(worker.is_stopped());
  const auto caller = std::this_thread::get_id();
  bool ran = false;
  bool was_cancelled = false;
  std::thread::id ran_on;
  TaskHandle handle = worker.post([&](bool cancelled) {
    ran = true;
    was_cancelled = cancelled;
    ran_on = std::this_thread::get_id();
  });
  EXPECT_FALSE(handle.valid());
  EXPECT_TRUE(ran);
  EXPECT_TRUE(was_cancelled);
  EXPECT_EQ(ran_on, caller);
}

}  // namespace
}  // namespace onpdf::engine
