#include "onpdf/core/engine/render_child.h"

#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/pdfium_library.h"
#include "onpdf/core/engine/render_protocol.h"
#include "onpdf/core/platform/ipc.h"
#include "onpdf/core/platform/path.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>

namespace onpdf::engine {

namespace {

namespace proto = render_protocol;

struct Fault {
  enum class Kind { kNone, kCrash, kHang } kind = Kind::kNone;
  int page = -1;
};

Fault parse_fault() {
  Fault f;
  const char* env = std::getenv("ONPDF_RENDER_CHILD_FAULT");
  if (env == nullptr) return f;
  const std::string s(env);
  const auto colon = s.find(':');
  if (colon == std::string::npos) return f;
  const std::string kind = s.substr(0, colon);
  f.page = std::atoi(s.c_str() + colon + 1);
  if (kind == "crash") f.kind = Fault::Kind::kCrash;
  if (kind == "hang") f.kind = Fault::Kind::kHang;
  return f;
}

[[noreturn]] void crash_now() {
#if defined(_MSC_VER)
  __debugbreak();
  std::abort();
#else
  __builtin_trap();
#endif
}

RenderOptions to_options(const proto::Render& r) {
  RenderOptions o;
  o.scale = r.scale;
  o.rotation = r.rotation;
  o.render_annotations = (r.flags & proto::kFlagAnnotations) != 0;
  o.lcd_text = (r.flags & proto::kFlagLcdText) != 0;
  o.printing = (r.flags & proto::kFlagPrinting) != 0;
  o.grayscale = (r.flags & proto::kFlagGrayscale) != 0;
  return o;
}

class Child {
 public:
  Child(platform::Pipe in, platform::Pipe out) : in_(std::move(in)), out_(std::move(out)) {
    fault_ = parse_fault();
  }

  int run() {
    const proto::ReadFn read = [this](void* p, std::size_t n) { return in_.read_exact(p, n); };
    for (;;) {
      Result<proto::Frame> frame = proto::read_frame(read);
      if (!frame) {
        if (frame.error().code == ErrorCode::kUnsupported) {
          // 버전이 다르면 알려 주고 끝낸다 — 부모가 폴백한다
          reply(0, proto::Error{static_cast<std::int32_t>(ErrorCode::kUnsupported),
                                frame.error().message});
        }
        return frame.error().code == ErrorCode::kIoError ? 0 : 3;  // 부모가 닫았다 = 정상 종료
      }
      const std::uint64_t id = frame->request_id;
      std::visit([this, id](auto& m) { handle(id, m); }, frame->message);
    }
  }

 private:
  void reply(std::uint64_t id, proto::Message m) {
    const proto::WriteFn write = [this](const void* p, std::size_t n) {
      return out_.write_all(p, n);
    };
    (void)proto::write_frame(write, proto::Frame{id, std::move(m)});
  }
  void reply_error(std::uint64_t id, const Error& e) {
    reply(id, proto::Error{static_cast<std::int32_t>(e.code), e.message});
  }

  void handle(std::uint64_t id, const proto::Open& m) {
    if (!library_) {
      PdfiumLibrary::Options options;
      options.use_system_fonts = m.use_system_fonts;
      library_.emplace(options);
    }
    document_.reset();
    password_ = m.password;
    auto doc = PdfiumDocument::open(onpdf::path_from_utf8(m.path), m.password);
    if (!doc) {
      reply_error(id, doc.error());
      return;
    }
    document_.emplace(std::move(*doc));
    reply(id, proto::Ok{});
  }

  void handle(std::uint64_t id, const proto::Replace& m) {
    if (!library_) {
      reply(id, proto::Error{static_cast<std::int32_t>(ErrorCode::kInvalidState), "open first"});
      return;
    }
    auto shm = platform::SharedMemory::open(m.shm_name, static_cast<std::size_t>(m.bytes));
    if (!shm) {
      reply_error(id, shm.error());
      return;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(m.bytes));
    std::memcpy(bytes.data(), shm->data(), bytes.size());
    auto doc = PdfiumDocument::open_from_memory(std::move(bytes), password_);
    if (!doc) {
      reply_error(id, doc.error());
      return;
    }
    document_.reset();
    document_.emplace(std::move(*doc));
    generation_ = m.generation;
    reply(id, proto::Ok{});
  }

  void handle(std::uint64_t id, const proto::Render& m) {
    if (!document_) {
      reply(id, proto::Error{static_cast<std::int32_t>(ErrorCode::kInvalidState), "no document"});
      return;
    }
    if (fault_.page == m.page) {
      if (fault_.kind == Fault::Kind::kCrash) crash_now();
      if (fault_.kind == Fault::Kind::kHang) {
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    }
    // 공유 메모리: 이름이 바뀌면 다시 연다
    if (!pixels_.valid() || pixels_.name() != m.shm_name || pixels_.size() != m.shm_bytes) {
      auto shm = platform::SharedMemory::open(m.shm_name, static_cast<std::size_t>(m.shm_bytes));
      if (!shm) {
        reply_error(id, shm.error());
        return;
      }
      pixels_ = std::move(*shm);
    }
    const auto t0 = std::chrono::steady_clock::now();
    const RenderOptions options = to_options(m);
    Result<Bitmap> bitmap =
        m.has_region
            ? document_->render_region(m.page, options,
                                       Rect{static_cast<double>(m.x), static_cast<double>(m.y),
                                            static_cast<double>(m.w), static_cast<double>(m.h)})
            : document_->render_page(m.page, options);
    if (!bitmap) {
      reply_error(id, bitmap.error());
      return;
    }
    const std::size_t needed =
        static_cast<std::size_t>(bitmap->stride) * static_cast<std::size_t>(bitmap->height);
    if (needed > pixels_.size()) {
      reply(id, proto::Error{static_cast<std::int32_t>(ErrorCode::kInvalidArgument),
                             "shared memory too small for " + std::to_string(needed) + " bytes"});
      return;
    }
    std::memcpy(pixels_.data(), bitmap->pixels.data(), needed);
    proto::Rendered r;
    r.width = bitmap->width;
    r.height = bitmap->height;
    r.stride = bitmap->stride;
    r.elapsed_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0)
            .count());
    reply(id, r);
  }

  void handle(std::uint64_t id, const proto::Close&) {
    document_.reset();
    pixels_.reset();
    reply(id, proto::Ok{});
  }
  void handle(std::uint64_t id, const proto::Ping&) { reply(id, proto::Ok{}); }
  // 부모가 보낼 일이 없는 메시지: 무시하되 응답은 한다
  void handle(std::uint64_t id, const proto::Ok&) { reply(id, proto::Ok{}); }
  void handle(std::uint64_t id, const proto::Rendered&) { reply(id, proto::Ok{}); }
  void handle(std::uint64_t id, const proto::Error&) { reply(id, proto::Ok{}); }

  platform::Pipe in_;
  platform::Pipe out_;
  std::optional<PdfiumLibrary> library_;
  std::optional<PdfiumDocument> document_;
  platform::SharedMemory pixels_;
  std::string password_;
  std::uint64_t generation_ = 0;
  Fault fault_;
};

}  // namespace

int run_render_child(int argc, char** argv) {
  if (argc < 4) return 2;
  platform::Pipe in = platform::Pipe::from_native_string(argv[2]);
  platform::Pipe out = platform::Pipe::from_native_string(argv[3]);
  if (!in.valid() || !out.valid()) return 2;
  platform::exit_when_parent_dies();  // 부모가 죽으면 멈춘 렌더 중이라도 끝난다 (#145)
  Child child(std::move(in), std::move(out));
  return child.run();
}

}  // namespace onpdf::engine
