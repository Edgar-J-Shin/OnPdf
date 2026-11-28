#pragma once

// 렌더 자식 프로세스 프로토콜 (#137). 부모(RenderProcess) ↔ 자식(run_render_child) 이 파이프로
// 주고받는 메시지. 고정 헤더 + 가변 페이로드(리틀 엔디언), 큰 데이터(문서 바이트·픽셀)는 공유
// 메모리로 넘기고 이름만 싣는다. Qt·PDFium 없음. I/O 는 콜백으로 주입해 테스트가 버퍼로 검증한다.

#include "onpdf/core/error.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace onpdf::engine::render_protocol {

constexpr std::uint32_t kMagic = 0x4350524Fu;  // 'OPRC' (리틀 엔디언으로 "OPRC")
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kMaxPayload = 64 * 1024;

enum class Kind : std::uint16_t {
  // 부모 → 자식
  kOpen = 1,
  kReplace = 2,
  kRender = 3,
  kClose = 4,
  kPing = 5,
  // 자식 → 부모
  kOk = 100,
  kRendered = 101,
  kError = 102,
};

// RenderOptions 의 불리언 플래그
constexpr std::uint32_t kFlagAnnotations = 1u << 0;
constexpr std::uint32_t kFlagLcdText = 1u << 1;
constexpr std::uint32_t kFlagPrinting = 1u << 2;
constexpr std::uint32_t kFlagGrayscale = 1u << 3;

struct Header {
  std::uint32_t magic = kMagic;
  std::uint16_t version = kVersion;
  std::uint16_t kind = 0;
  std::uint32_t payload_bytes = 0;
  std::uint64_t request_id = 0;
};
static_assert(sizeof(Header) == 24);

struct Open {
  bool use_system_fonts = true;
  std::string path;      // UTF-8
  std::string password;  // UTF-8
};
struct Replace {
  std::uint64_t generation = 0;
  std::string shm_name;
  std::uint64_t bytes = 0;
};
struct Render {
  std::uint64_t generation = 0;
  std::int32_t page = 0;
  float scale = 1.0f;
  std::int32_t rotation = 0;
  std::uint32_t flags = kFlagAnnotations;
  bool has_region = false;  // false: 쪽 전체 (render_page), true: region (render_region)
  std::int32_t x = 0, y = 0, w = 0, h = 0;
  std::string shm_name;  // 픽셀을 쓸 공유 메모리
  std::uint64_t shm_bytes = 0;
};
struct Close {};
struct Ping {};
struct Ok {};
struct Rendered {
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::int32_t stride = 0;
  std::uint64_t elapsed_ns = 0;
};
struct Error {
  std::int32_t code = 0;  // ErrorCode
  std::string message;
};

using Message = std::variant<Open, Replace, Render, Close, Ping, Ok, Rendered, Error>;

struct Frame {
  std::uint64_t request_id = 0;
  Message message;
};

// ---- 직렬화 ----
namespace detail {

class Writer {
 public:
  void u8(std::uint8_t v) { buf_.push_back(v); }
  void u32(std::uint32_t v) { raw(&v, sizeof v); }
  void i32(std::int32_t v) { raw(&v, sizeof v); }
  void u64(std::uint64_t v) { raw(&v, sizeof v); }
  void f32(float v) { raw(&v, sizeof v); }
  void str(const std::string& s) {
    u32(static_cast<std::uint32_t>(s.size()));
    buf_.insert(buf_.end(), s.begin(), s.end());
  }
  std::vector<std::uint8_t> take() { return std::move(buf_); }

 private:
  void raw(const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    buf_.insert(buf_.end(), b, b + n);
  }
  std::vector<std::uint8_t> buf_;
};

class Reader {
 public:
  Reader(const std::uint8_t* data, std::size_t size) : p_(data), end_(data + size) {}
  bool u8(std::uint8_t& v) { return raw(&v, 1); }
  bool u32(std::uint32_t& v) { return raw(&v, sizeof v); }
  bool i32(std::int32_t& v) { return raw(&v, sizeof v); }
  bool u64(std::uint64_t& v) { return raw(&v, sizeof v); }
  bool f32(float& v) { return raw(&v, sizeof v); }
  bool str(std::string& s) {
    std::uint32_t n = 0;
    if (!u32(n) || static_cast<std::size_t>(end_ - p_) < n) return false;
    s.assign(reinterpret_cast<const char*>(p_), n);
    p_ += n;
    return true;
  }
  [[nodiscard]] bool done() const { return p_ == end_; }

 private:
  bool raw(void* out, std::size_t n) {
    if (static_cast<std::size_t>(end_ - p_) < n) return false;
    std::memcpy(out, p_, n);
    p_ += n;
    return true;
  }
  const std::uint8_t* p_;
  const std::uint8_t* end_;
};

}  // namespace detail

inline Kind kind_of(const Message& m) {
  return static_cast<Kind>(std::visit(
      [](const auto& v) -> std::uint16_t {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, Open>) return 1;
        if constexpr (std::is_same_v<T, Replace>) return 2;
        if constexpr (std::is_same_v<T, Render>) return 3;
        if constexpr (std::is_same_v<T, Close>) return 4;
        if constexpr (std::is_same_v<T, Ping>) return 5;
        if constexpr (std::is_same_v<T, Ok>) return 100;
        if constexpr (std::is_same_v<T, Rendered>) return 101;
        return 102;
      },
      m));
}

inline std::vector<std::uint8_t> encode_payload(const Message& m) {
  detail::Writer w;
  std::visit(
      [&w](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, Open>) {
          w.u8(v.use_system_fonts ? 1 : 0);
          w.str(v.path);
          w.str(v.password);
        } else if constexpr (std::is_same_v<T, Replace>) {
          w.u64(v.generation);
          w.str(v.shm_name);
          w.u64(v.bytes);
        } else if constexpr (std::is_same_v<T, Render>) {
          w.u64(v.generation);
          w.i32(v.page);
          w.f32(v.scale);
          w.i32(v.rotation);
          w.u32(v.flags);
          w.u8(v.has_region ? 1 : 0);
          w.i32(v.x);
          w.i32(v.y);
          w.i32(v.w);
          w.i32(v.h);
          w.str(v.shm_name);
          w.u64(v.shm_bytes);
        } else if constexpr (std::is_same_v<T, Rendered>) {
          w.i32(v.width);
          w.i32(v.height);
          w.i32(v.stride);
          w.u64(v.elapsed_ns);
        } else if constexpr (std::is_same_v<T, Error>) {
          w.i32(v.code);
          w.str(v.message);
        }
      },
      m);
  return w.take();
}

inline std::optional<Message> decode_payload(Kind kind, const std::uint8_t* data,
                                             std::size_t size) {
  detail::Reader r(data, size);
  std::uint8_t b = 0;
  switch (kind) {
    case Kind::kOpen: {
      Open m;
      if (!r.u8(b) || !r.str(m.path) || !r.str(m.password) || !r.done()) return std::nullopt;
      m.use_system_fonts = b != 0;
      return m;
    }
    case Kind::kReplace: {
      Replace m;
      if (!r.u64(m.generation) || !r.str(m.shm_name) || !r.u64(m.bytes) || !r.done())
        return std::nullopt;
      return m;
    }
    case Kind::kRender: {
      Render m;
      if (!r.u64(m.generation) || !r.i32(m.page) || !r.f32(m.scale) || !r.i32(m.rotation) ||
          !r.u32(m.flags) || !r.u8(b) || !r.i32(m.x) || !r.i32(m.y) || !r.i32(m.w) || !r.i32(m.h) ||
          !r.str(m.shm_name) || !r.u64(m.shm_bytes) || !r.done())
        return std::nullopt;
      m.has_region = b != 0;
      return m;
    }
    case Kind::kClose:
      return r.done() ? std::optional<Message>(Close{}) : std::nullopt;
    case Kind::kPing:
      return r.done() ? std::optional<Message>(Ping{}) : std::nullopt;
    case Kind::kOk:
      return r.done() ? std::optional<Message>(Ok{}) : std::nullopt;
    case Kind::kRendered: {
      Rendered m;
      if (!r.i32(m.width) || !r.i32(m.height) || !r.i32(m.stride) || !r.u64(m.elapsed_ns) ||
          !r.done())
        return std::nullopt;
      return m;
    }
    case Kind::kError: {
      Error m;
      if (!r.i32(m.code) || !r.str(m.message) || !r.done()) return std::nullopt;
      return m;
    }
  }
  return std::nullopt;
}

// I/O 주입: write(data, n) / read(out, n) 는 정확히 n 바이트를 처리하면 true
using WriteFn = std::function<bool(const void*, std::size_t)>;
using ReadFn = std::function<bool(void*, std::size_t)>;

inline bool write_frame(const WriteFn& write, const Frame& frame) {
  const std::vector<std::uint8_t> payload = encode_payload(frame.message);
  if (payload.size() > kMaxPayload) return false;
  Header h;
  h.kind = static_cast<std::uint16_t>(kind_of(frame.message));
  h.payload_bytes = static_cast<std::uint32_t>(payload.size());
  h.request_id = frame.request_id;
  if (!write(&h, sizeof h)) return false;
  return payload.empty() || write(payload.data(), payload.size());
}

// 상대가 닫혔으면 kIoError, 형식이 틀리면 kInvalidFormat, 버전이 다르면 kUnsupported
inline Result<Frame> read_frame(const ReadFn& read) {
  Header h;
  if (!read(&h, sizeof h)) return make_error(ErrorCode::kIoError, "pipe closed");
  if (h.magic != kMagic) return make_error(ErrorCode::kInvalidFormat, "bad magic");
  if (h.version != kVersion) {
    return make_error(ErrorCode::kUnsupported, "protocol version " + std::to_string(h.version) +
                                                   " != " + std::to_string(kVersion));
  }
  if (h.payload_bytes > kMaxPayload)
    return make_error(ErrorCode::kInvalidFormat, "payload too big");
  std::vector<std::uint8_t> payload(h.payload_bytes);
  if (h.payload_bytes > 0 && !read(payload.data(), payload.size()))
    return make_error(ErrorCode::kIoError, "pipe closed");
  auto message = decode_payload(static_cast<Kind>(h.kind), payload.data(), payload.size());
  if (!message) return make_error(ErrorCode::kInvalidFormat, "bad payload");
  return Frame{h.request_id, std::move(*message)};
}

}  // namespace onpdf::engine::render_protocol
