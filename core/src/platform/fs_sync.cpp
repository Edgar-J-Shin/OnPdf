#include "onpdf/core/platform/fs_sync.h"

#include "onpdf/core/platform/path.h"

#include <string>

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
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

namespace onpdf::platform {
namespace {

#ifdef _WIN32

Error windows_error(const std::string& what, const std::filesystem::path& path) {
  const DWORD code = GetLastError();
  ErrorCode kind = ErrorCode::kIoError;
  switch (code) {
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_WRITE_PROTECT:
      kind = ErrorCode::kNotWritable;
      break;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
      kind = ErrorCode::kFileNotFound;
      break;
    default:
      break;
  }
  return Error{kind, what + " " + path_to_utf8(path) + " (Windows error " + std::to_string(code) + ")"};
}

// HANDLE RAII
class FileHandle {
 public:
  explicit FileHandle(HANDLE handle) : handle_(handle) {}
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;
  ~FileHandle() {
    if (valid()) CloseHandle(handle_);
  }
  [[nodiscard]] bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }
  [[nodiscard]] HANDLE get() const noexcept { return handle_; }

 private:
  HANDLE handle_;
};

#else

Error posix_error(const std::string& what, const std::filesystem::path& path, int err) {
  ErrorCode kind = ErrorCode::kIoError;
  switch (err) {
    case EACCES:
    case EPERM:
    case EROFS:
    case EBUSY:
    case ETXTBSY:
      kind = ErrorCode::kNotWritable;
      break;
    case ENOENT:
      kind = ErrorCode::kFileNotFound;
      break;
    default:
      break;
  }
  return Error{kind, what + " " + path_to_utf8(path) + ": " + std::strerror(err)};
}

// 파일 디스크립터 RAII
class Fd {
 public:
  explicit Fd(int fd) : fd_(fd) {}
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  ~Fd() {
    if (fd_ >= 0) ::close(fd_);
  }
  [[nodiscard]] int get() const noexcept { return fd_; }

 private:
  int fd_;
};

#endif

}  // namespace

Result<void> flush_file_to_disk(const std::filesystem::path& file) {
#ifdef _WIN32
  FileHandle handle(CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!handle.valid()) return std::unexpected(windows_error("cannot open for flush:", file));
  if (!FlushFileBuffers(handle.get())) return std::unexpected(windows_error("flush failed:", file));
  return {};
#else
  // 읽기 전용으로 연다: fsync 는 쓰기 권한이 필요 없다. 원본 권한(예: 소유자 쓰기 없음)을 옮긴 뒤에도 동작한다
  Fd fd(::open(file.c_str(), O_RDONLY | O_CLOEXEC));
  if (fd.get() < 0) return std::unexpected(posix_error("cannot open for flush:", file, errno));
#ifdef F_FULLFSYNC
  // macOS: fsync 는 드라이브 캐시까지 내리지 않는다. 지원하지 않는 파일 시스템(일부 네트워크)이면 fsync 로
  if (::fcntl(fd.get(), F_FULLFSYNC) == 0) return {};
#endif
  if (::fsync(fd.get()) != 0) return std::unexpected(posix_error("fsync failed:", file, errno));
  return {};
#endif
}

Result<void> replace_file(const std::filesystem::path& from, const std::filesystem::path& to) {
#ifdef _WIN32
  if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return std::unexpected(windows_error("cannot replace", to));
  }
  return {};
#else
  if (::rename(from.c_str(), to.c_str()) != 0) return std::unexpected(posix_error("cannot replace", to, errno));
  return {};
#endif
}

Result<void> flush_directory(const std::filesystem::path& dir) {
#ifdef _WIN32
  (void)dir;  // MoveFileExW(MOVEFILE_WRITE_THROUGH) 가 이미 이름 바꾸기를 디스크까지 내린다
  return {};
#else
  Fd fd(::open(dir.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY));
  if (fd.get() < 0) return std::unexpected(posix_error("cannot open folder:", dir, errno));
  if (::fsync(fd.get()) != 0) return std::unexpected(posix_error("folder fsync failed:", dir, errno));
  return {};
#endif
}

}  // namespace onpdf::platform
