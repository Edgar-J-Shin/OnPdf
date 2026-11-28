#include "onpdf/core/engine/render_process.h"

#include "onpdf/core/engine/render_protocol.h"
#include "onpdf/core/platform/ipc.h"
#include "onpdf/core/platform/path.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace onpdf::engine {

namespace proto = render_protocol;

struct RenderProcessImpl {
  enum class Kind { kOpen, kOpenMemory, kClose, kReplace, kRender };

  struct Entry {
    Kind kind = Kind::kRender;
    std::shared_ptr<std::atomic<bool>> cancelled;  // 렌더만
    std::uint64_t generation = 0;                  // 렌더·Replace
    int page = -1;
    RenderOptions options;
    std::optional<Rect> region;
    std::function<void(Result<Bitmap>)> on_done;
    // 제어 메시지 페이로드
    std::filesystem::path path;
    std::string password;
    std::vector<std::uint8_t> bytes;
  };

  explicit RenderProcessImpl(RenderProcess::Options o) : options(std::move(o)) {}

  RenderProcess::Options options;

  // ---- 여러 스레드: mutex ----
  mutable std::mutex mutex;
  std::condition_variable cv;
  std::deque<Entry> control;                // Open/Close/Replace — 렌더보다 먼저
  std::array<std::deque<Entry>, 3> queues;  // index = Priority
  std::uint64_t pending_generation = 0;     // begin_resync 가 올린다
  std::uint64_t applied_generation = 0;     // Replace 가 자식에 적용되면
  bool stopping = false;
  std::atomic<bool> stop_requested{false};  // 소멸자가 켠다 — 응답 대기를 짧게 끊는다
  RenderProcess::EventCallback on_event;
  RenderProcess::Stats stats;
  std::unordered_set<int> refused_pages;

  // ---- 클라이언트 스레드 전용 ----
  std::optional<platform::ChildProcess> child;
  platform::Pipe to_child;
  platform::Pipe from_child;
  platform::SharedMemory pixels;
  std::uint64_t next_request_id = 1;
  int spawn_failures = 0;
  std::unordered_map<int, int> page_failures;
  // 재시작 때 다시 보낼 문서 지정
  struct Source {
    bool from_memory = false;
    std::filesystem::path path;
    std::string password;
    std::vector<std::uint8_t> bytes;  // from_memory 또는 마지막 Replace
    std::uint64_t generation = 0;     // 마지막 Replace 세대
  };
  std::optional<Source> source;

  std::thread thread;

  static TaskHandle enqueue_render(RenderProcessImpl& impl, int page, const RenderOptions& options,
                                   std::optional<Rect> region, Priority priority,
                                   std::function<void(Result<Bitmap>)> on_done);

  // ------------------------------------------------------------ 큐
  void push_control(Entry e) {
    {
      std::lock_guard lock(mutex);
      control.push_back(std::move(e));
    }
    cv.notify_one();
  }

  // 다음 항목: 제어 먼저, 렌더는 세대가 적용된 것만. 없으면 대기
  bool pop_next(Entry& out) {
    std::unique_lock lock(mutex);
    for (;;) {
      if (!control.empty()) {
        out = std::move(control.front());
        control.pop_front();
        return true;
      }
      for (int p = 2; p >= 0; --p) {
        auto& q = queues[static_cast<std::size_t>(p)];
        for (auto it = q.begin(); it != q.end(); ++it) {
          if (it->generation <= applied_generation ||
              it->cancelled->load(std::memory_order_relaxed)) {
            out = std::move(*it);
            q.erase(it);
            return true;
          }
        }
      }
      if (stopping) return false;
      cv.wait(lock);
    }
  }

  void drain_all(ErrorCode code, const std::string& message) {
    std::deque<Entry> leftovers;
    {
      std::lock_guard lock(mutex);
      for (auto& q : queues) {
        for (auto& e : q) leftovers.push_back(std::move(e));
        q.clear();
      }
      control.clear();
    }
    for (auto& e : leftovers) {
      if (e.on_done) e.on_done(make_error(code, message));
    }
  }

  void emit(RenderProcess::Event::Kind kind, int page, std::string reason) {
    RenderProcess::EventCallback cb;
    {
      std::lock_guard lock(mutex);
      cb = on_event;
    }
    if (cb) cb(RenderProcess::Event{kind, page, std::move(reason)});
  }

  // ------------------------------------------------------------ 자식
  bool ensure_child() {
    if (child && child->alive()) return true;
    if (child) {  // 죽은 자식 정리
      child->wait(std::chrono::milliseconds(0));
      child.reset();
      to_child.close();
      from_child.close();
    }
    if (unavailable()) return false;
    auto parent_to_child = platform::Pipe::create();
    auto child_to_parent = platform::Pipe::create();
    if (!parent_to_child || !child_to_parent) return spawn_failed("pipe");
    platform::ChildProcess::Spawn spawn;
    spawn.exe = options.exe;
    spawn.child_read = parent_to_child->read.native();
    spawn.child_write = child_to_parent->write.native();
    const auto [in_str, out_str] = platform::ChildProcess::child_handle_strings(spawn);
    spawn.args = {"--render-child", in_str, out_str};
    if (!options.report_dir.empty()) {
      spawn.env.emplace_back("ONPDF_REPORT_DIR", onpdf::path_to_utf8(options.report_dir));
    }
    auto spawned = platform::ChildProcess::spawn(spawn);
    if (!spawned) return spawn_failed(spawned.error().message);
    child.emplace(std::move(*spawned));
    to_child = std::move(parent_to_child->write);
    from_child = std::move(child_to_parent->read);
    parent_to_child->read.close();  // 자식이 상속한 끝은 부모가 닫는다 (EOF 전달용)
    child_to_parent->write.close();
    // 문서를 다시 지정
    if (source) {
      bool ok = false;
      if (source->from_memory) {
        ok = send_bytes(proto::Replace{source->generation, {}, 0}, source->bytes, source->password,
                        true);
      } else {
        ok = roundtrip_ok(proto::Open{options.use_system_fonts, onpdf::path_to_utf8(source->path),
                                      source->password},
                          options.control_timeout);
        if (ok && !source->bytes.empty()) {
          ok = send_bytes(proto::Replace{source->generation, {}, 0}, source->bytes,
                          source->password, false);
        }
      }
      if (!ok) return spawn_failed("document re-open failed");
    }
    spawn_failures = 0;
    return true;
  }

  bool spawn_failed(const std::string& why) {
    ++spawn_failures;
    if (spawn_failures >= options.max_spawn_failures) {
      {
        std::lock_guard lock(mutex);
        stats.unavailable = true;
      }
      emit(RenderProcess::Event::Kind::kSpawnFailed, -1, why);
    }
    return false;
  }

  bool unavailable() const {
    std::lock_guard lock(mutex);
    return stats.unavailable;
  }

  // 바이트를 공유 메모리로 넘겨 Replace (from_memory 첫 열기도 Replace 로 — 자식은 Open 없이
  // Replace 가 오면 library 가 없으므로 먼저 빈 Open? → 라이브러리 생성을 위해 Open 을 보낸다)
  bool send_bytes(proto::Replace replace, const std::vector<std::uint8_t>& bytes,
                  const std::string& password, bool needs_library) {
    if (needs_library) {
      // 자식 라이브러리 초기화 + 암호 등록: 존재하지 않는 경로로 Open → 실패 응답은 무시한다
      (void)roundtrip(proto::Open{options.use_system_fonts, "", password}, options.control_timeout);
    }
    auto shm = platform::SharedMemory::create(platform::SharedMemory::unique_name(),
                                              std::max<std::size_t>(bytes.size(), 1));
    if (!shm) return false;
    std::memcpy(shm->data(), bytes.data(), bytes.size());
    replace.shm_name = shm->name();
    replace.bytes = bytes.size();
    return roundtrip_ok(replace, options.control_timeout);
  }

  // 메시지 보내고 응답 대기. nullopt = 자식 사망·시간 초과 (호출자가 kill·재시작)
  std::optional<proto::Message> roundtrip(proto::Message m, std::chrono::milliseconds timeout) {
    const std::uint64_t id = next_request_id++;
    const proto::WriteFn write = [this](const void* p, std::size_t n) {
      return to_child.write_all(p, n);
    };
    if (!proto::write_frame(write, proto::Frame{id, std::move(m)})) return std::nullopt;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      // 100ms 조각으로 기다려 소멸(앱 종료) 요청에 바로 응한다 — 자식이 렌더 중이어도 예산까지
      // 기다리지 않는다 (호출자가 kill 한다)
      platform::Pipe::Wait wait = platform::Pipe::Wait::kTimeout;
      while (wait == platform::Pipe::Wait::kTimeout) {
        if (stop_requested.load(std::memory_order_relaxed)) return std::nullopt;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return std::nullopt;
        wait = from_child.wait_readable(std::min(left, std::chrono::milliseconds(100)));
      }
      if (wait != platform::Pipe::Wait::kReadable) return std::nullopt;
      const proto::ReadFn read = [this](void* p, std::size_t n) {
        return from_child.read_exact(p, n);
      };
      auto frame = proto::read_frame(read);
      if (!frame) return std::nullopt;
      if (frame->request_id != id) continue;  // 늦은 응답은 버린다
      return std::move(frame->message);
    }
  }

  bool roundtrip_ok(proto::Message m, std::chrono::milliseconds timeout) {
    auto reply = roundtrip(std::move(m), timeout);
    return reply && std::holds_alternative<proto::Ok>(*reply);
  }

  void kill_child(const char* reason) {
    (void)reason;
    if (child) {
      child->kill();
      child->wait(std::chrono::milliseconds(2000));
      child.reset();
    }
    to_child.close();
    from_child.close();
  }

  // ------------------------------------------------------------ 처리
  void handle_control(Entry& e) {
    switch (e.kind) {
      case Kind::kOpen:
        source = Source{false, e.path, e.password, {}, 0};
        {
          std::lock_guard lock(mutex);
          refused_pages.clear();
          applied_generation = pending_generation;  // 새 문서: 보류 없음
        }
        page_failures.clear();
        if (!ensure_child()) return;
        if (!roundtrip_ok(
                proto::Open{options.use_system_fonts, onpdf::path_to_utf8(e.path), e.password},
                options.control_timeout)) {
          kill_child("open failed");
        }
        break;
      case Kind::kOpenMemory:
        source = Source{true, {}, e.password, std::move(e.bytes), 0};
        {
          std::lock_guard lock(mutex);
          refused_pages.clear();
          applied_generation = pending_generation;
        }
        page_failures.clear();
        if (!ensure_child()) return;
        if (!send_bytes(proto::Replace{0, {}, 0}, source->bytes, source->password, true))
          kill_child("open from memory failed");
        break;
      case Kind::kReplace:
        if (source) {
          source->bytes = std::move(e.bytes);
          source->generation = e.generation;
        }
        if (ensure_child() && source && !source->bytes.empty()) {
          if (!send_bytes(proto::Replace{e.generation, {}, 0}, source->bytes, source->password,
                          false)) {
            kill_child("replace failed");
          }
        }
        {
          // 자식이 죽었더라도 세대는 적용으로 본다 — 재시작이 source 로 같은 바이트를 올린다
          std::lock_guard lock(mutex);
          applied_generation = std::max(applied_generation, e.generation);
        }
        cv.notify_all();
        break;
      case Kind::kClose:
        source.reset();
        if (child && child->alive()) (void)roundtrip_ok(proto::Close{}, options.control_timeout);
        break;
      case Kind::kRender:
        break;
    }
  }

  void handle_render(Entry& e) {
    if (e.cancelled->load(std::memory_order_relaxed)) {
      e.on_done(make_error(ErrorCode::kCancelled, "task cancelled"));
      return;
    }
    {
      std::lock_guard lock(mutex);
      if (refused_pages.contains(e.page)) {
        e.on_done(make_error(ErrorCode::kRenderRefused,
                             "page " + std::to_string(e.page + 1) +
                                 ": the render process failed repeatedly on this page"));
        return;
      }
    }
    if (!ensure_child()) {
      e.on_done(make_error(ErrorCode::kRenderProcessLost, "render process unavailable"));
      return;
    }
    // 필요한 픽셀 크기
    const double s = e.options.scale;
    std::size_t needed = 0;
    if (e.region) {
      const int w = static_cast<int>(std::ceil(e.region->w));
      const int h = static_cast<int>(std::ceil(e.region->h));
      needed =
          static_cast<std::size_t>(std::max(1, w)) * static_cast<std::size_t>(std::max(1, h)) * 4;
    } else {
      needed = static_cast<std::size_t>(64) << 20;  // 쪽 전체: 크기를 모른다 → 상한 64MB
      (void)s;
    }
    if (!pixels.valid() || pixels.size() < needed) {
      auto shm = platform::SharedMemory::create(platform::SharedMemory::unique_name(),
                                                std::max<std::size_t>(needed, 4 * 1024 * 1024));
      if (!shm) {
        e.on_done(make_error(ErrorCode::kRenderProcessLost, "shared memory unavailable"));
        return;
      }
      pixels = std::move(*shm);
    }
    proto::Render r;
    r.generation = e.generation;
    r.page = e.page;
    r.scale = e.options.scale;
    r.rotation = e.options.rotation;
    r.flags = (e.options.render_annotations ? proto::kFlagAnnotations : 0u) |
              (e.options.lcd_text ? proto::kFlagLcdText : 0u) |
              (e.options.printing ? proto::kFlagPrinting : 0u) |
              (e.options.grayscale ? proto::kFlagGrayscale : 0u);
    if (e.region) {
      r.has_region = true;
      r.x = static_cast<int>(std::floor(e.region->x));
      r.y = static_cast<int>(std::floor(e.region->y));
      r.w = static_cast<int>(std::ceil(e.region->w));
      r.h = static_cast<int>(std::ceil(e.region->h));
    }
    r.shm_name = pixels.name();
    r.shm_bytes = pixels.size();
    auto reply = roundtrip(r, options.timeout);
    if (!reply) {
      // EOF 는 프로세스가 거둬지기 조금 전에 올 수 있다 → 잠깐 기다려 죽음과 시간 초과를 가른다
      const bool died = child && child->wait(std::chrono::milliseconds(300)) >= 0;
      const std::string reason = died ? "crashed" : "timeout";
      kill_child(reason.c_str());
      const int failures = ++page_failures[e.page];
      bool refuse = failures >= options.max_page_failures;
      {
        std::lock_guard lock(mutex);
        ++stats.restarts;
        if (refuse) refused_pages.insert(e.page);
      }
      emit(RenderProcess::Event::Kind::kRestarted, e.page, reason);
      if (refuse) {
        emit(RenderProcess::Event::Kind::kRefused, e.page, reason);
        e.on_done(make_error(ErrorCode::kRenderRefused, "page " + std::to_string(e.page + 1) +
                                                            ": render process " + reason + " (" +
                                                            std::to_string(failures) + "x)"));
      } else {
        e.on_done(make_error(ErrorCode::kRenderProcessLost, "render process " + reason));
      }
      return;
    }
    if (auto* err = std::get_if<proto::Error>(&*reply)) {
      e.on_done(make_error(static_cast<ErrorCode>(err->code), err->message));
      return;
    }
    auto* rendered = std::get_if<proto::Rendered>(&*reply);
    if (rendered == nullptr || rendered->width <= 0 || rendered->height <= 0) {
      e.on_done(make_error(ErrorCode::kUnknown, "unexpected reply"));
      return;
    }
    Bitmap bitmap;
    bitmap.width = rendered->width;
    bitmap.height = rendered->height;
    bitmap.stride = rendered->stride;
    const std::size_t bytes =
        static_cast<std::size_t>(bitmap.stride) * static_cast<std::size_t>(bitmap.height);
    if (bytes > pixels.size()) {
      e.on_done(make_error(ErrorCode::kUnknown, "reply exceeds shared memory"));
      return;
    }
    bitmap.pixels.resize(bytes);
    std::memcpy(bitmap.pixels.data(), pixels.data(), bytes);
    {
      std::lock_guard lock(mutex);
      ++stats.rendered;
    }
    e.on_done(std::move(bitmap));
  }

  void run() {
    Entry e;
    while (pop_next(e)) {
      if (e.kind == Kind::kRender) {
        handle_render(e);
      } else {
        handle_control(e);
      }
      e = Entry{};
    }
    // 종료: 자식에 EOF → 1초 안에 안 끝나면 kill
    to_child.close();
    if (child) {
      if (child->wait(std::chrono::milliseconds(1000)) < 0) child->kill();
      child.reset();
    }
    from_child.close();
    pixels.reset();
  }
};

// ================================================================= RenderProcess

RenderProcess::RenderProcess(Options options)
    : impl_(std::make_shared<RenderProcessImpl>(std::move(options))) {
  std::shared_ptr<RenderProcessImpl> impl = impl_;
  impl_->thread = std::thread([impl] { impl->run(); });
}

RenderProcess::~RenderProcess() {
  impl_->stop_requested.store(true, std::memory_order_relaxed);
  {
    std::lock_guard lock(impl_->mutex);
    impl_->stopping = true;
  }
  impl_->cv.notify_all();
  // 남은 요청은 취소 통지
  impl_->drain_all(ErrorCode::kCancelled, "render process stopping");
  if (impl_->thread.joinable()) impl_->thread.join();
}

void RenderProcess::open(std::filesystem::path path, std::string password) {
  RenderProcessImpl::Entry e;
  e.kind = RenderProcessImpl::Kind::kOpen;
  e.path = std::move(path);
  e.password = std::move(password);
  impl_->push_control(std::move(e));
}

void RenderProcess::open_from_memory(std::vector<std::uint8_t> bytes, std::string password) {
  RenderProcessImpl::Entry e;
  e.kind = RenderProcessImpl::Kind::kOpenMemory;
  e.bytes = std::move(bytes);
  e.password = std::move(password);
  impl_->push_control(std::move(e));
}

void RenderProcess::close() {
  RenderProcessImpl::Entry e;
  e.kind = RenderProcessImpl::Kind::kClose;
  impl_->push_control(std::move(e));
}

std::uint64_t RenderProcess::begin_resync() {
  std::lock_guard lock(impl_->mutex);
  return ++impl_->pending_generation;
}

void RenderProcess::replace(std::uint64_t generation, std::vector<std::uint8_t> bytes) {
  RenderProcessImpl::Entry e;
  e.kind = RenderProcessImpl::Kind::kReplace;
  e.generation = generation;
  e.bytes = std::move(bytes);
  impl_->push_control(std::move(e));
}

TaskHandle RenderProcessImpl::enqueue_render(RenderProcessImpl& impl, int page,
                                             const RenderOptions& options,
                                             std::optional<Rect> region, Priority priority,
                                             std::function<void(Result<Bitmap>)> on_done) {
  assert(on_done && "콜백은 비어 있을 수 없다");
  auto flag = std::make_shared<std::atomic<bool>>(false);
  RenderProcessImpl::Entry e;
  e.kind = RenderProcessImpl::Kind::kRender;
  e.cancelled = flag;
  e.page = page;
  e.options = options;
  e.region = region;
  e.on_done = std::move(on_done);
  {
    std::lock_guard lock(impl.mutex);
    if (impl.stopping) {
      flag->store(true);
      e.on_done(make_error(ErrorCode::kCancelled, "render process stopping"));
      return TaskHandle{};
    }
    e.generation = impl.pending_generation;
    impl.queues[static_cast<std::size_t>(priority)].push_back(std::move(e));
  }
  impl.cv.notify_one();
  return TaskHandle(std::move(flag));
}

TaskHandle RenderProcess::render_page(int page_index, const RenderOptions& options,
                                      Priority priority,
                                      std::function<void(Result<Bitmap>)> on_done) {
  return RenderProcessImpl::enqueue_render(*impl_, page_index, options, std::nullopt, priority,
                                           std::move(on_done));
}

TaskHandle RenderProcess::render_region(int page_index, const RenderOptions& options,
                                        const Rect& region_px, Priority priority,
                                        std::function<void(Result<Bitmap>)> on_done) {
  return RenderProcessImpl::enqueue_render(*impl_, page_index, options, region_px, priority,
                                           std::move(on_done));
}

void RenderProcess::set_on_event(EventCallback callback) {
  std::lock_guard lock(impl_->mutex);
  impl_->on_event = std::move(callback);
}

RenderProcess::Stats RenderProcess::stats() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->stats;
}

bool RenderProcess::unavailable() const noexcept {
  return impl_->unavailable();
}

const RenderProcess::Options& RenderProcess::options() const noexcept {
  return impl_->options;
}

}  // namespace onpdf::engine
