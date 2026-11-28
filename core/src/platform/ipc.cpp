#include "onpdf/core/platform/ipc.h"

#include "onpdf/core/platform/path.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace onpdf::platform {

// ================================================================= Pipe

Pipe& Pipe::operator=(Pipe&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.release();
  }
  return *this;
}

NativeHandle Pipe::release() noexcept {
  const NativeHandle h = handle_;
  handle_ = kInvalidHandle;
  return h;
}

std::string Pipe::native_string() const {
#ifdef _WIN32
  return std::to_string(reinterpret_cast<std::uintptr_t>(handle_));
#else
  return std::to_string(handle_);
#endif
}

Pipe Pipe::from_native_string(const std::string& text) {
  if (text.empty()) return Pipe{};
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') return Pipe{};
#ifdef _WIN32
  return Pipe(reinterpret_cast<NativeHandle>(static_cast<std::uintptr_t>(value)));
#else
  return Pipe(static_cast<int>(value));
#endif
}

#ifdef _WIN32

Result<PipePair> Pipe::create() {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof sa;
  sa.bInheritHandle = FALSE;  // 상속은 spawn 이 필요한 끝만 켠다
  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
    return make_error(ErrorCode::kIoError, "CreatePipe failed: " + std::to_string(GetLastError()));
  }
  PipePair pair;
  pair.read = Pipe(read_end);
  pair.write = Pipe(write_end);
  return pair;
}

void Pipe::close() noexcept {
  if (handle_ != kInvalidHandle && handle_ != nullptr) CloseHandle(handle_);
  handle_ = kInvalidHandle;
}

bool Pipe::read_exact(void* out, std::size_t n) {
  auto* p = static_cast<char*>(out);
  while (n > 0) {
    DWORD got = 0;
    const DWORD chunk = static_cast<DWORD>(n > 0x7fffffff ? 0x7fffffff : n);
    if (!ReadFile(handle_, p, chunk, &got, nullptr) || got == 0) return false;
    p += got;
    n -= got;
  }
  return true;
}

bool Pipe::write_all(const void* data, std::size_t n) {
  const auto* p = static_cast<const char*>(data);
  while (n > 0) {
    DWORD put = 0;
    const DWORD chunk = static_cast<DWORD>(n > 0x7fffffff ? 0x7fffffff : n);
    if (!WriteFile(handle_, p, chunk, &put, nullptr) || put == 0) return false;
    p += put;
    n -= put;
  }
  return true;
}

Pipe::Wait Pipe::wait_readable(std::chrono::milliseconds timeout) {
  // 익명 파이프는 대기 객체가 아니다 → PeekNamedPipe 폴링 (1ms). 상대가 닫히면 BROKEN_PIPE
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    DWORD available = 0;
    if (!PeekNamedPipe(handle_, nullptr, 0, nullptr, &available, nullptr)) return Wait::kClosed;
    if (available > 0) return Wait::kReadable;
    if (std::chrono::steady_clock::now() >= deadline) return Wait::kTimeout;
    Sleep(1);
  }
}

#else  // POSIX

Result<PipePair> Pipe::create() {
  int fds[2];
  if (::pipe(fds) != 0) return make_error(ErrorCode::kIoError, "pipe failed");
  // 자식에는 spawn 이 dup2 로 넘기는 두 끝만 보여야 한다
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  PipePair pair;
  pair.read = Pipe(fds[0]);
  pair.write = Pipe(fds[1]);
  return pair;
}

void Pipe::close() noexcept {
  if (handle_ >= 0) ::close(handle_);
  handle_ = kInvalidHandle;
}

bool Pipe::read_exact(void* out, std::size_t n) {
  auto* p = static_cast<char*>(out);
  while (n > 0) {
    const ssize_t r = ::read(handle_, p, n);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return false;
    p += r;
    n -= static_cast<std::size_t>(r);
  }
  return true;
}

bool Pipe::write_all(const void* data, std::size_t n) {
  const auto* p = static_cast<const char*>(data);
  while (n > 0) {
    const ssize_t w = ::write(handle_, p, n);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return false;
    p += w;
    n -= static_cast<std::size_t>(w);
  }
  return true;
}

Pipe::Wait Pipe::wait_readable(std::chrono::milliseconds timeout) {
  pollfd pfd{handle_, POLLIN, 0};
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    const int ms = left.count() <= 0 ? 0 : static_cast<int>(left.count());
    const int rc = ::poll(&pfd, 1, ms);
    if (rc < 0 && errno == EINTR) continue;
    if (rc == 0) return Wait::kTimeout;
    if (rc < 0) return Wait::kClosed;
    if (pfd.revents & POLLIN) return Wait::kReadable;
    return Wait::kClosed;  // POLLHUP/POLLERR
  }
}

#endif

// ================================================================= SharedMemory

SharedMemory::SharedMemory(SharedMemory&& other) noexcept
    : data_(other.data_), size_(other.size_), name_(std::move(other.name_)), owner_(other.owner_) {
#ifdef _WIN32
  mapping_ = other.mapping_;
  other.mapping_ = kInvalidHandle;
#endif
  other.data_ = nullptr;
  other.size_ = 0;
  other.owner_ = false;
}

SharedMemory& SharedMemory::operator=(SharedMemory&& other) noexcept {
  if (this != &other) {
    reset();
    data_ = other.data_;
    size_ = other.size_;
    name_ = std::move(other.name_);
    owner_ = other.owner_;
#ifdef _WIN32
    mapping_ = other.mapping_;
    other.mapping_ = kInvalidHandle;
#endif
    other.data_ = nullptr;
    other.size_ = 0;
    other.owner_ = false;
  }
  return *this;
}

SharedMemory::~SharedMemory() {
  reset();
}

std::string SharedMemory::unique_name() {
  static std::atomic<std::uint64_t> counter{0};
  return "onpdf-r-" + std::to_string(current_process_id()) + "-" +
         std::to_string(counter.fetch_add(1) + 1);
}

#ifdef _WIN32

namespace {
std::wstring mapping_name(const std::string& name) {
  const std::wstring w = onpdf::path_from_utf8(name).wstring();
  return L"Local\\" + w;
}
}  // namespace

Result<SharedMemory> SharedMemory::create(const std::string& name, std::size_t bytes) {
  const std::wstring wname = mapping_name(name);
  HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                      static_cast<DWORD>(static_cast<std::uint64_t>(bytes) >> 32),
                                      static_cast<DWORD>(bytes & 0xffffffffu), wname.c_str());
  if (mapping == nullptr) {
    return make_error(ErrorCode::kIoError,
                      "CreateFileMapping failed: " + std::to_string(GetLastError()));
  }
  void* data = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
  if (data == nullptr) {
    CloseHandle(mapping);
    return make_error(ErrorCode::kIoError, "MapViewOfFile failed");
  }
  SharedMemory shm;
  shm.data_ = data;
  shm.size_ = bytes;
  shm.name_ = name;
  shm.owner_ = true;
  shm.mapping_ = mapping;
  return shm;
}

Result<SharedMemory> SharedMemory::open(const std::string& name, std::size_t bytes) {
  const std::wstring wname = mapping_name(name);
  HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wname.c_str());
  if (mapping == nullptr) {
    return make_error(ErrorCode::kIoError,
                      "OpenFileMapping failed: " + std::to_string(GetLastError()));
  }
  void* data = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
  if (data == nullptr) {
    CloseHandle(mapping);
    return make_error(ErrorCode::kIoError, "MapViewOfFile failed");
  }
  SharedMemory shm;
  shm.data_ = data;
  shm.size_ = bytes;
  shm.name_ = name;
  shm.owner_ = false;
  shm.mapping_ = mapping;
  return shm;
}

void SharedMemory::reset() noexcept {
  if (data_ != nullptr) UnmapViewOfFile(data_);
  if (mapping_ != kInvalidHandle && mapping_ != nullptr) CloseHandle(mapping_);
  mapping_ = kInvalidHandle;
  data_ = nullptr;
  size_ = 0;
  owner_ = false;
  name_.clear();
}

#else

namespace {
std::string shm_path(const std::string& name) {
  return "/" +
         name;  // shm_open 은 선행 '/' 이름 (macOS 는 31자 상한 — "onpdf-r-<pid>-<seq>" 는 안)
}
}  // namespace

Result<SharedMemory> SharedMemory::create(const std::string& name, std::size_t bytes) {
  const std::string path = shm_path(name);
  shm_unlink(path.c_str());
  const int fd = shm_open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) return make_error(ErrorCode::kIoError, "shm_open(create) failed: " + path);
  if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
    ::close(fd);
    shm_unlink(path.c_str());
    return make_error(ErrorCode::kIoError, "ftruncate failed");
  }
  void* data = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (data == MAP_FAILED) {
    shm_unlink(path.c_str());
    return make_error(ErrorCode::kIoError, "mmap failed");
  }
  SharedMemory shm;
  shm.data_ = data;
  shm.size_ = bytes;
  shm.name_ = name;
  shm.owner_ = true;
  return shm;
}

Result<SharedMemory> SharedMemory::open(const std::string& name, std::size_t bytes) {
  const std::string path = shm_path(name);
  const int fd = shm_open(path.c_str(), O_RDWR, 0600);
  if (fd < 0) return make_error(ErrorCode::kIoError, "shm_open failed: " + path);
  void* data = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (data == MAP_FAILED) return make_error(ErrorCode::kIoError, "mmap failed");
  SharedMemory shm;
  shm.data_ = data;
  shm.size_ = bytes;
  shm.name_ = name;
  shm.owner_ = false;
  return shm;
}

void SharedMemory::reset() noexcept {
  if (data_ != nullptr) munmap(data_, size_);
  if (owner_ && !name_.empty()) shm_unlink(shm_path(name_).c_str());
  data_ = nullptr;
  size_ = 0;
  owner_ = false;
  name_.clear();
}

#endif

// ================================================================= ChildProcess

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : pid_(other.pid_), exit_code_(other.exit_code_), reaped_(other.reaped_) {
#ifdef _WIN32
  process_ = other.process_;
  job_ = other.job_;
  other.process_ = kInvalidHandle;
  other.job_ = kInvalidHandle;
#endif
  other.pid_ = 0;
  other.reaped_ = true;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    this->~ChildProcess();
    new (this) ChildProcess(std::move(other));
  }
  return *this;
}

#ifdef _WIN32

std::pair<std::string, std::string> ChildProcess::child_handle_strings(Spawn& spawn) {
  return {std::to_string(reinterpret_cast<std::uintptr_t>(spawn.child_read)),
          std::to_string(reinterpret_cast<std::uintptr_t>(spawn.child_write))};
}

Result<ChildProcess> ChildProcess::spawn(const Spawn& spawn) {
  // 상속되는 핸들은 두 끝만: PROC_THREAD_ATTRIBUTE_HANDLE_LIST
  HANDLE inherit[2] = {spawn.child_read, spawn.child_write};
  for (HANDLE h : inherit) SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
  std::vector<char> attr_buf(attr_size);
  auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
  if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size)) {
    return make_error(ErrorCode::kIoError, "InitializeProcThreadAttributeList failed");
  }
  if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                 sizeof inherit, nullptr, nullptr)) {
    DeleteProcThreadAttributeList(attrs);
    return make_error(ErrorCode::kIoError, "UpdateProcThreadAttribute failed");
  }
  // 명령줄: "exe" arg1 arg2 … (ASCII 인자만이라 따옴표 처리는 exe 경로만)
  std::wstring cmd = L"\"" + spawn.exe.wstring() + L"\"";
  for (const auto& a : spawn.args) {
    cmd += L" ";
    cmd += onpdf::path_from_utf8(a).wstring();
  }
  // 환경: 현재 환경 + 추가 변수
  std::wstring env_block;
  {
    LPWCH current = GetEnvironmentStringsW();
    for (const wchar_t* p = current; *p != L'\0';) {
      const std::wstring entry(p);
      env_block += entry;
      env_block.push_back(L'\0');
      p += entry.size() + 1;
    }
    FreeEnvironmentStringsW(current);
    for (const auto& [key, value] : spawn.env) {
      env_block += onpdf::path_from_utf8(key + "=" + value).wstring();
      env_block.push_back(L'\0');
    }
    env_block.push_back(L'\0');
  }
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof si;
  si.lpAttributeList = attrs;
  PROCESS_INFORMATION pi{};
  const BOOL ok = CreateProcessW(spawn.exe.wstring().c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                 EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
                                     CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                 env_block.data(), nullptr, &si.StartupInfo, &pi);
  DeleteProcThreadAttributeList(attrs);
  for (HANDLE h : inherit) SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0);
  if (!ok) {
    return make_error(ErrorCode::kIoError,
                      "CreateProcess failed: " + std::to_string(GetLastError()));
  }
  // Job 객체: 부모가 죽으면 자식도 끝난다 (고아 방지)
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job != nullptr) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    AssignProcessToJobObject(job, pi.hProcess);
  }
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  ChildProcess child;
  child.pid_ = static_cast<long long>(pi.dwProcessId);
  child.process_ = pi.hProcess;
  child.job_ = job != nullptr ? job : kInvalidHandle;
  child.reaped_ = false;
  return child;
}

ChildProcess::~ChildProcess() {
  if (pid_ != 0 && !reaped_) {
    kill();
    wait(std::chrono::milliseconds(2000));
  }
  if (process_ != kInvalidHandle && process_ != nullptr) CloseHandle(process_);
  if (job_ != kInvalidHandle && job_ != nullptr) CloseHandle(job_);  // KILL_ON_JOB_CLOSE
}

bool ChildProcess::alive() {
  if (pid_ == 0 || reaped_) return false;
  return WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

void ChildProcess::kill() noexcept {
  if (pid_ != 0 && !reaped_) TerminateProcess(process_, 0xC000DEADu);
}

int ChildProcess::wait(std::chrono::milliseconds timeout) {
  if (pid_ == 0) return -1;
  if (reaped_) return exit_code_;
  if (WaitForSingleObject(process_, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0)
    return -1;
  DWORD code = 0;
  GetExitCodeProcess(process_, &code);
  exit_code_ = static_cast<int>(code);
  reaped_ = true;
  return exit_code_;
}

long long current_process_id() noexcept {
  return static_cast<long long>(GetCurrentProcessId());
}

#else  // POSIX

std::pair<std::string, std::string> ChildProcess::child_handle_strings(Spawn& spawn) {
  // 소스 fd 와 겹치지 않는 번호 두 개 (dup2 순서에 따라 소스가 덮어쓰이는 사고 방지)
  int fd = 10;
  const auto taken = [&spawn](int f) { return f == spawn.child_read || f == spawn.child_write; };
  while (taken(fd)) ++fd;
  spawn.posix_read_fd = fd++;
  while (taken(fd)) ++fd;
  spawn.posix_write_fd = fd;
  return {std::to_string(spawn.posix_read_fd), std::to_string(spawn.posix_write_fd)};
}

Result<ChildProcess> ChildProcess::spawn(const Spawn& spawn) {
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  // 높은 번호로 복제 — 3·4 로 하면 부모의 파이프 fd 와 겹쳐 닫기 순서에 따라 끝이 닫힌다 (#135
  // 교훈)
  posix_spawn_file_actions_adddup2(&actions, spawn.child_read, spawn.posix_read_fd);
  posix_spawn_file_actions_adddup2(&actions, spawn.child_write, spawn.posix_write_fd);
  // 그 밖의 fd 는 FD_CLOEXEC 로 만들어져 있다 (Pipe::create). dup2 된 끝은 CLOEXEC 가 없다
  std::vector<std::string> argv_storage;
  argv_storage.push_back(spawn.exe.string());
  for (const auto& a : spawn.args) argv_storage.push_back(a);
  std::vector<char*> argv;
  for (auto& s : argv_storage) argv.push_back(s.data());
  argv.push_back(nullptr);
  // 환경: 현재 + 추가
  std::vector<std::string> env_storage;
  for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
    bool overridden = false;
    for (const auto& [key, value] : spawn.env) {
      if (std::strncmp(*e, key.c_str(), key.size()) == 0 && (*e)[key.size()] == '=') {
        overridden = true;
        break;
      }
    }
    if (!overridden) env_storage.emplace_back(*e);
  }
  for (const auto& [key, value] : spawn.env) env_storage.push_back(key + "=" + value);
  std::vector<char*> envp;
  for (auto& s : env_storage) envp.push_back(s.data());
  envp.push_back(nullptr);
  pid_t pid = 0;
  const int rc =
      posix_spawn(&pid, argv_storage.front().c_str(), &actions, nullptr, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  if (rc != 0) return make_error(ErrorCode::kIoError, "posix_spawn failed: " + std::to_string(rc));
  ChildProcess child;
  child.pid_ = static_cast<long long>(pid);
  child.reaped_ = false;
  return child;
}

ChildProcess::~ChildProcess() {
  if (pid_ != 0 && !reaped_) {
    kill();
    wait(std::chrono::milliseconds(2000));
  }
}

bool ChildProcess::alive() {
  if (pid_ == 0 || reaped_) return false;
  int status = 0;
  const pid_t r = waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
  if (r == 0) return true;
  reaped_ = true;
  exit_code_ = WIFSIGNALED(status) ? 128 + WTERMSIG(status) : WEXITSTATUS(status);
  return false;
}

void ChildProcess::kill() noexcept {
  if (pid_ != 0 && !reaped_) ::kill(static_cast<pid_t>(pid_), SIGKILL);
}

int ChildProcess::wait(std::chrono::milliseconds timeout) {
  if (pid_ == 0) return -1;
  if (reaped_) return exit_code_;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    if (!alive()) return exit_code_;
    if (std::chrono::steady_clock::now() >= deadline) return -1;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

long long current_process_id() noexcept {
  return static_cast<long long>(getpid());
}

#endif

}  // namespace onpdf::platform
