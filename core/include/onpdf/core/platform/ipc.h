#pragma once

// 프로세스 간 통신의 플랫폼 계층 (#137 렌더 자식 프로세스). 플랫폼 분기(#ifdef _WIN32)는
// ipc.cpp 안에만 둔다. Qt 없음.
//  - Pipe: 익명 파이프의 한쪽 끝. 블로킹 read_exact/write_all 과 시간 제한 대기
//  - SharedMemory: 이름 있는 공유 메모리(픽셀·문서 바이트). 만든 쪽이 지운다
//  - ChildProcess: 자식 띄우기(파이프 두 끝만 상속)·생존 확인·kill·종료 대기. Windows 는 Job
//    객체로 부모가 죽으면 자식도 끝난다
//  - exit_when_parent_dies: 자식 쪽에서 부른다. POSIX 는 Job 객체가 없어 부모가 크래시·_Exit 로
//    죽으면 멈춘 자식(렌더 폭탄·장애 주입)이 고아로 남아 파이프를 쥔다(#145: ctest 가 EOF 를 못
//    받는다) → 감시 스레드가 getppid() 변화를 보고 프로세스를 끝낸다. Windows 는 아무것도 안 한다

#include "onpdf/core/error.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace onpdf::platform {

#ifdef _WIN32
using NativeHandle = void*;  // HANDLE
inline const NativeHandle kInvalidHandle = reinterpret_cast<void*>(-1);
#else
using NativeHandle = int;  // fd
inline constexpr NativeHandle kInvalidHandle = -1;
#endif

struct PipePair;

class Pipe {
 public:
  // 익명 파이프 한 쌍 (읽기 끝, 쓰기 끝)
  static Result<PipePair> create();
  // 자식이 명령줄 인자로 받은 핸들 값(10진)에서 만든다. 잘못된 값이면 invalid
  static Pipe from_native_string(const std::string& text);

  Pipe() = default;
  explicit Pipe(NativeHandle handle) : handle_(handle) {}
  Pipe(Pipe&& other) noexcept : handle_(other.release()) {}
  Pipe& operator=(Pipe&& other) noexcept;
  Pipe(const Pipe&) = delete;
  Pipe& operator=(const Pipe&) = delete;
  ~Pipe() { close(); }

  [[nodiscard]] bool valid() const noexcept { return handle_ != kInvalidHandle; }
  [[nodiscard]] NativeHandle native() const noexcept { return handle_; }
  [[nodiscard]] std::string native_string() const;  // 자식 인자용 10진 문자열
  NativeHandle release() noexcept;
  void close() noexcept;

  // 정확히 n 바이트. 상대가 닫혔거나 오류면 false
  bool read_exact(void* out, std::size_t n);
  bool write_all(const void* data, std::size_t n);

  enum class Wait { kReadable, kTimeout, kClosed };
  // 읽을 데이터가 생기거나 상대가 닫힐 때까지 최대 timeout. 자식이 죽어 쓰기 끝이 닫히면 kClosed
  // (또는 kReadable 뒤 read_exact 가 false)
  Wait wait_readable(std::chrono::milliseconds timeout);

 private:
  NativeHandle handle_ = kInvalidHandle;
};

struct PipePair {
  Pipe read;
  Pipe write;
};

class SharedMemory {
 public:
  static Result<SharedMemory> create(const std::string& name, std::size_t bytes);  // 소유(지운다)
  static Result<SharedMemory> open(const std::string& name, std::size_t bytes);    // 참조만

  SharedMemory() = default;
  SharedMemory(SharedMemory&& other) noexcept;
  SharedMemory& operator=(SharedMemory&& other) noexcept;
  SharedMemory(const SharedMemory&) = delete;
  SharedMemory& operator=(const SharedMemory&) = delete;
  ~SharedMemory();

  [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }
  [[nodiscard]] void* data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  void reset() noexcept;

  // 이 프로세스에서 고유한 이름 ("onpdf-r-<pid>-<n>", 프로세스 전역 카운터 — 여러 RenderProcess 가
  // 같은 프로세스에 있어도 겹치지 않는다. 플랫폼 접두는 구현이 붙인다)
  static std::string unique_name();

 private:
  void* data_ = nullptr;
  std::size_t size_ = 0;
  std::string name_;
  bool owner_ = false;
#ifdef _WIN32
  NativeHandle mapping_ = kInvalidHandle;
#endif
};

class ChildProcess {
 public:
  struct Spawn {
    std::filesystem::path exe;
    std::vector<std::string> args;              // argv[1..] — ASCII 만 (Windows 의 ANSI argv)
    NativeHandle child_read = kInvalidHandle;   // 자식이 읽는 끝 (부모 → 자식)
    NativeHandle child_write = kInvalidHandle;  // 자식이 쓰는 끝 (자식 → 부모)
    std::vector<std::pair<std::string, std::string>> env;  // 추가 환경 변수 (UTF-8)
    // POSIX: 자식에서 두 끝이 갖게 될 fd 번호. child_handle_strings 가 부모의 소스 fd 와 겹치지
    // 않게 고른다 (dup2 가 소스를 덮어쓰지 않도록 — 한 프로세스에 자식이 여럿이면 10·11 도 겹칠 수
    // 있다)
    int posix_read_fd = 10;
    int posix_write_fd = 11;
  };
  // 자식에는 child_read·child_write 만 상속된다. 먼저 child_handle_strings(spawn) 으로 자식에 넘길
  // 핸들 문자열을 받아 args 에 넣고 spawn 한다
  static Result<ChildProcess> spawn(const Spawn& spawn);
  // 자식에게 보일 핸들 값 (10진) — POSIX 는 겹치지 않는 fd 번호를 골라 spawn.posix_*_fd 에 기록,
  // Windows 는 상속된 HANDLE 값
  [[nodiscard]] static std::pair<std::string, std::string> child_handle_strings(Spawn& spawn);

  ChildProcess() = default;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ~ChildProcess();  // 살아 있으면 kill + 거둔다
  void swap(ChildProcess& other) noexcept;

  [[nodiscard]] bool valid() const noexcept { return pid_ != 0; }
  [[nodiscard]] long long pid() const noexcept { return pid_; }
  [[nodiscard]] bool alive();
  void kill() noexcept;
  // 끝날 때까지 최대 timeout. 끝났으면 종료 코드(신호면 128+번호), 아니면 -1
  int wait(std::chrono::milliseconds timeout);

 private:
  long long pid_ = 0;
  int exit_code_ = -1;
  bool reaped_ = false;
#ifdef _WIN32
  NativeHandle process_ = kInvalidHandle;
  NativeHandle job_ = kInvalidHandle;
#endif
};

[[nodiscard]] long long current_process_id() noexcept;

// 부모 프로세스가 죽으면 이 프로세스를 즉시 끝내는 감시 스레드를 시작한다 (위 설명)
void exit_when_parent_dies();

}  // namespace onpdf::platform
