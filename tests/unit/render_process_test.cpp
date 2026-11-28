// 렌더 자식 프로세스 (#137): 프로토콜 직렬화, IPC 기본, RenderProcess 클라이언트(픽셀
// 동일·우선순위· 취소·세대 보류·크래시 재시작·2회 거부·시간 초과·큰 요청·실행 파일 없음). 자식 실행
// 파일은 이 테스트 바이너리 자신이다(main 이 --render-child 면 run_render_child 로 분기).
// PdfiumLibrary 는 쓰지 않는다 (자식 프로세스가 각자 만든다) — 기준 픽셀은 별도 자식(또는
// in-process 비교용 전용 자식)으로 얻는다.

#include "onpdf/core/engine/render_process.h"

#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/pdfium_library.h"
#include "onpdf/core/engine/render_child.h"
#include "onpdf/core/engine/render_protocol.h"
#include "onpdf/core/platform/ipc.h"
#include "onpdf/core/platform/path.h"

#include "../test_support.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <gtest/gtest.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace onpdf::engine {
namespace {

using namespace std::chrono_literals;
namespace proto = render_protocol;
using onpdf::test::testdata;

std::filesystem::path self_exe;  // main 이 채운다

// ---------------------------------------------------------------- 프로토콜

TEST(RenderProtocol, RoundTripsEveryMessage) {
  std::vector<std::uint8_t> buffer;
  const proto::WriteFn write = [&buffer](const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    buffer.insert(buffer.end(), b, b + n);
    return true;
  };
  std::size_t pos = 0;
  const proto::ReadFn read = [&buffer, &pos](void* out, std::size_t n) {
    if (buffer.size() - pos < n) return false;
    std::memcpy(out, buffer.data() + pos, n);
    pos += n;
    return true;
  };
  proto::Render render;
  render.generation = 7;
  render.page = 3;
  render.scale = 1.5f;
  render.rotation = 90;
  render.flags = proto::kFlagAnnotations | proto::kFlagPrinting;
  render.has_region = true;
  render.x = 512;
  render.y = -3;
  render.w = 512;
  render.h = 100;
  render.shm_name = "onpdf-r-1-2";
  render.shm_bytes = 1 << 20;
  const std::vector<proto::Frame> frames = {
      {1, proto::Open{false, "/tmp/한글 파일명.pdf", "pw"}},
      {2, proto::Replace{9, "onpdf-r-1-3", 12345}},
      {3, render},
      {4, proto::Close{}},
      {5, proto::Ping{}},
      {6, proto::Ok{}},
      {7, proto::Rendered{512, 100, 2048, 123456}},
      {8, proto::Error{static_cast<std::int32_t>(ErrorCode::kRenderRefused), "page 2: bomb"}},
  };
  for (const auto& f : frames) ASSERT_TRUE(proto::write_frame(write, f));
  for (const auto& expected : frames) {
    auto got = proto::read_frame(read);
    ASSERT_TRUE(got.has_value()) << got.error().message;
    EXPECT_EQ(got->request_id, expected.request_id);
    EXPECT_EQ(proto::kind_of(got->message), proto::kind_of(expected.message));
    EXPECT_EQ(proto::encode_payload(got->message), proto::encode_payload(expected.message));
  }
  EXPECT_FALSE(proto::read_frame(read).has_value());  // 닫힘
  // 버전 불일치·손상
  proto::Header bad;
  bad.version = 99;
  bad.kind = 5;
  buffer.assign(reinterpret_cast<const std::uint8_t*>(&bad),
                reinterpret_cast<const std::uint8_t*>(&bad) + sizeof bad);
  pos = 0;
  auto r = proto::read_frame(read);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrorCode::kUnsupported);
  bad.version = proto::kVersion;
  bad.magic = 1;
  buffer.assign(reinterpret_cast<const std::uint8_t*>(&bad),
                reinterpret_cast<const std::uint8_t*>(&bad) + sizeof bad);
  pos = 0;
  EXPECT_EQ(proto::read_frame(read).error().code, ErrorCode::kInvalidFormat);
}

// ---------------------------------------------------------------- IPC

TEST(Ipc, PipeAndSharedMemory) {
  auto pair = platform::Pipe::create();
  ASSERT_TRUE(pair.has_value());
  EXPECT_EQ(pair->read.wait_readable(20ms), platform::Pipe::Wait::kTimeout);
  const char hello[] = "hello";
  ASSERT_TRUE(pair->write.write_all(hello, sizeof hello));
  EXPECT_EQ(pair->read.wait_readable(1000ms), platform::Pipe::Wait::kReadable);
  char got[sizeof hello] = {};
  ASSERT_TRUE(pair->read.read_exact(got, sizeof got));
  EXPECT_STREQ(got, "hello");
  pair->write.close();
  EXPECT_FALSE(pair->read.read_exact(got, 1));  // EOF

  const std::string name = platform::SharedMemory::unique_name();
  auto owner = platform::SharedMemory::create(name, 4096);
  ASSERT_TRUE(owner.has_value()) << owner.error().message;
  std::memcpy(owner->data(), "shared", 7);
  auto other = platform::SharedMemory::open(name, 4096);
  ASSERT_TRUE(other.has_value()) << other.error().message;
  EXPECT_STREQ(static_cast<const char*>(other->data()), "shared");
  other->reset();
  owner->reset();
  EXPECT_FALSE(platform::SharedMemory::open(name, 4096).has_value());  // 지워졌다
}

// ---------------------------------------------------------------- 클라이언트

Result<Bitmap> render_sync(RenderProcess& rp, int page, const RenderOptions& options,
                           std::optional<Rect> region, Priority priority = Priority::kNormal) {
  std::promise<Result<Bitmap>> promise;
  auto future = promise.get_future();
  auto cb = [&promise](Result<Bitmap> r) { promise.set_value(std::move(r)); };
  if (region) {
    rp.render_region(page, options, *region, priority, cb);
  } else {
    rp.render_page(page, options, priority, cb);
  }
  if (future.wait_for(60s) != std::future_status::ready) {
    return make_error(ErrorCode::kUnknown, "test timeout");
  }
  return future.get();
}

RenderProcess::Options options_for(std::chrono::milliseconds timeout = 20000ms) {
  RenderProcess::Options o;
  o.exe = self_exe;
  o.use_system_fonts = false;
  o.timeout = timeout;
  return o;
}

class RenderProcessTest : public ::testing::Test {
 protected:
  void SetUp() override { unsetenv("ONPDF_RENDER_CHILD_FAULT"); }
  void TearDown() override { unsetenv("ONPDF_RENDER_CHILD_FAULT"); }
  std::vector<RenderProcess::Event> events(RenderProcess& rp) {
    std::lock_guard lock(mutex_);
    (void)rp;
    return events_;
  }
  void capture(RenderProcess& rp) {
    rp.set_on_event([this](const RenderProcess::Event& e) {
      std::lock_guard lock(mutex_);
      events_.push_back(e);
    });
  }
  std::mutex mutex_;
  std::vector<RenderProcess::Event> events_;
};

TEST_F(RenderProcessTest, RendersSamePixelsAsAnotherChild) {
  // 두 자식이 같은 타일을 그리면 같다 (결정성) — 기준 PNG 와의 비교는 렌더 회귀 테스트가 한다
  RenderProcess a(options_for());
  RenderProcess b(options_for());
  a.open(testdata("korean-embedded.pdf"), "");
  b.open(testdata("korean-embedded.pdf"), "");
  RenderOptions opts;
  opts.scale = 2.0f;
  const Rect tile{0, 0, 512, 512};
  auto ra = render_sync(a, 0, opts, tile);
  auto rb = render_sync(b, 0, opts, tile);
  ASSERT_TRUE(ra.has_value()) << ra.error().message;
  ASSERT_TRUE(rb.has_value()) << rb.error().message;
  EXPECT_EQ(ra->width, 512);
  EXPECT_EQ(ra->height, 512);
  EXPECT_EQ(ra->pixels, rb->pixels);
  // 쪽 전체
  auto page = render_sync(a, 0, RenderOptions{}, std::nullopt);
  ASSERT_TRUE(page.has_value()) << page.error().message;
  EXPECT_GT(page->width, 100);
  EXPECT_EQ(a.stats().rendered, 2u);
  EXPECT_EQ(a.stats().restarts, 0);
}

TEST_F(RenderProcessTest, HonoursPriorityAndCancel) {
  RenderProcess rp(options_for());
  rp.open(testdata("large-1000.pdf"), "");
  // 먼저 큐를 채워 두고(자식이 Open 을 처리하는 동안) 순서를 본다
  std::mutex m;
  std::vector<int> order;
  std::promise<void> done;
  int remaining = 3;
  auto cb_for = [&](int tag) {
    return [&, tag](Result<Bitmap>) {
      std::lock_guard lock(m);
      order.push_back(tag);
      if (--remaining == 0) done.set_value();
    };
  };
  RenderOptions opts;
  opts.scale = 0.5f;
  TaskHandle low = rp.render_page(1, opts, Priority::kLow, cb_for(1));
  TaskHandle cancelled = rp.render_page(2, opts, Priority::kLow, [&](Result<Bitmap> r) {
    std::lock_guard lock(m);
    EXPECT_FALSE(r.has_value());
    if (!r) EXPECT_EQ(r.error().code, ErrorCode::kCancelled);
    order.push_back(-2);
    if (--remaining == 0) done.set_value();
  });
  cancelled.cancel();
  rp.render_page(3, opts, Priority::kHigh, cb_for(3));
  remaining = 3;
  ASSERT_EQ(done.get_future().wait_for(60s), std::future_status::ready);
  // High(3) 가 Low(1) 보다 먼저, 취소(−2)는 렌더 없이 통지
  ASSERT_EQ(order.size(), 3u);
  const auto pos = [&](int tag) {
    return std::find(order.begin(), order.end(), tag) - order.begin();
  };
  EXPECT_LT(pos(3), pos(1));
  EXPECT_TRUE(std::find(order.begin(), order.end(), -2) != order.end());
}

TEST_F(RenderProcessTest, ReplaceGenerationGatesRenders) {
  RenderProcess rp(options_for());
  rp.open(testdata("simple.pdf"), "");
  RenderOptions opts;
  opts.scale = 1.0f;
  auto before = render_sync(rp, 0, opts, std::nullopt);
  ASSERT_TRUE(before.has_value());
  // 다른 문서 바이트로 교체 — 세대를 올린 뒤 낸 렌더는 교체 뒤에 처리된다
  const std::uint64_t gen = rp.begin_resync();
  std::promise<Result<Bitmap>> promise;
  auto future = promise.get_future();
  rp.render_page(0, opts, Priority::kHigh,
                 [&promise](Result<Bitmap> r) { promise.set_value(std::move(r)); });
  EXPECT_EQ(future.wait_for(300ms), std::future_status::timeout);  // 보류 중
  const auto bytes = onpdf::test::read_bytes(testdata("korean-embedded.pdf"));
  rp.replace(gen, bytes);
  ASSERT_EQ(future.wait_for(60s), std::future_status::ready);
  auto after = future.get();
  ASSERT_TRUE(after.has_value()) << after.error().message;
  EXPECT_NE(after->pixels, before->pixels);  // 새 문서의 내용
}

TEST_F(RenderProcessTest, CrashRestartsAndRefusesAfterTwoFailures) {
  setenv("ONPDF_RENDER_CHILD_FAULT", "crash:1", 1);  // 2쪽(인덱스 1)에서 죽는다
  RenderProcess rp(options_for());
  capture(rp);
  rp.open(testdata("simple.pdf"), "");
  RenderOptions opts;
  auto ok = render_sync(rp, 0, opts, std::nullopt);
  ASSERT_TRUE(ok.has_value()) << ok.error().message;
  auto first = render_sync(rp, 1, opts, std::nullopt);
  ASSERT_FALSE(first.has_value());
  EXPECT_EQ(first.error().code, ErrorCode::kRenderProcessLost);
  // 재시작 뒤 다른 쪽은 그려진다
  auto again = render_sync(rp, 0, opts, std::nullopt);
  ASSERT_TRUE(again.has_value()) << again.error().message;
  EXPECT_EQ(again->pixels, ok->pixels);
  auto second = render_sync(rp, 1, opts, std::nullopt);
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().code, ErrorCode::kRenderRefused);
  auto third = render_sync(rp, 1, opts, std::nullopt);  // 더는 자식에 보내지 않는다
  ASSERT_FALSE(third.has_value());
  EXPECT_EQ(third.error().code, ErrorCode::kRenderRefused);
  const auto stats = rp.stats();
  EXPECT_EQ(stats.restarts, 2);
  EXPECT_FALSE(stats.unavailable);
  const auto ev = events(rp);
  EXPECT_GE(ev.size(), 3u);  // restarted, restarted, refused
  EXPECT_TRUE(std::any_of(ev.begin(), ev.end(), [](const RenderProcess::Event& e) {
    return e.kind == RenderProcess::Event::Kind::kRefused && e.page == 1;
  }));
}

TEST_F(RenderProcessTest, HangIsKilledAfterTimeout) {
  setenv("ONPDF_RENDER_CHILD_FAULT", "hang:0", 1);
  RenderProcess rp(options_for(700ms));
  capture(rp);
  rp.open(testdata("simple.pdf"), "");
  const auto t0 = std::chrono::steady_clock::now();
  auto r = render_sync(rp, 0, RenderOptions{}, std::nullopt);
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrorCode::kRenderProcessLost);
  EXPECT_NE(r.error().message.find("timeout"), std::string::npos);
  EXPECT_LT(elapsed, 10s);
  // 재시작 뒤 1쪽은 또 멈추지만 다른 쪽은 그려진다 (장애 주입은 0쪽만)
  auto other = render_sync(rp, 1, RenderOptions{}, std::nullopt);
  ASSERT_TRUE(other.has_value()) << other.error().message;
  EXPECT_EQ(rp.stats().restarts, 1);
}

TEST_F(RenderProcessTest, LargeRequestGrowsSharedMemory) {
  RenderProcess rp(options_for());
  rp.open(testdata("simple.pdf"), "");
  RenderOptions opts;
  opts.scale = 4.0f;  // 612×792pt → 2448×3168 px ≈ 31MB
  opts.printing = true;
  auto r = render_sync(rp, 0, opts, std::nullopt);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->width, 2448);
  EXPECT_EQ(r->height, 3168);
  // 이어서 작은 타일도 정상
  auto tile = render_sync(rp, 0, RenderOptions{}, Rect{0, 0, 64, 64});
  ASSERT_TRUE(tile.has_value());
  EXPECT_EQ(tile->width, 64);
}

TEST_F(RenderProcessTest, MissingExecutableBecomesUnavailable) {
  RenderProcess::Options o = options_for();
  o.exe = self_exe.parent_path() / "does-not-exist-onpdf";
  o.max_spawn_failures = 2;
  RenderProcess rp(o);
  capture(rp);
  rp.open(testdata("simple.pdf"), "");
  auto r = render_sync(rp, 0, RenderOptions{}, std::nullopt);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrorCode::kRenderProcessLost);
  auto r2 = render_sync(rp, 0, RenderOptions{}, std::nullopt);
  ASSERT_FALSE(r2.has_value());
  EXPECT_TRUE(rp.unavailable());
  const auto ev = events(rp);
  EXPECT_TRUE(std::any_of(ev.begin(), ev.end(), [](const RenderProcess::Event& e) {
    return e.kind == RenderProcess::Event::Kind::kSpawnFailed;
  }));
}

TEST_F(RenderProcessTest, HazardPagesAreRefusedByChildToo) {
  RenderProcess rp(options_for());
  rp.open(testdata("pattern-bomb.pdf"), "");
  auto safe = render_sync(rp, 0, RenderOptions{}, std::nullopt);
  ASSERT_TRUE(safe.has_value()) << safe.error().message;
  auto bomb = render_sync(rp, 1, RenderOptions{}, std::nullopt);
  ASSERT_FALSE(bomb.has_value());
  EXPECT_EQ(bomb.error().code, ErrorCode::kRenderRefused);
  EXPECT_EQ(rp.stats().restarts, 0);  // 사전 검사가 막아 자식은 죽지 않는다
}

}  // namespace
}  // namespace onpdf::engine

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "--render-child") == 0) {
    return onpdf::engine::run_render_child(argc, argv);
  }
  onpdf::engine::self_exe = std::filesystem::absolute(argv[0]);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
