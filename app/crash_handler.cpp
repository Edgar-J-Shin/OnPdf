#include "crash_handler.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <ctime>
#include <iterator>
#include <map>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX  // windows.h 의 min/max 매크로가 std::max 를 깨뜨린다
#endif
#include <windows.h>
// windows.h 뒤
#include <dbghelp.h>
#else
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#if defined(__APPLE__) || defined(__GLIBC__)
#include <execinfo.h>  // backtrace / backtrace_symbols_fd (malloc 없음)
#endif
#endif

namespace onpdf::app::crash {

namespace {

// ---- 설치 때 채우는 고정 버퍼 (핸들러는 읽기만) ----
constexpr std::size_t kPathCap = 1024;
constexpr std::size_t kVersionCap = 64;
constexpr std::size_t kActionCap = 64;
constexpr std::size_t kRecordCap = 512;

bool g_installed = false;
std::atomic<bool> g_handling{false};    // 핸들러 재진입 차단 (핸들러 안의 크래시·재발)
char g_report_dir_utf8[kPathCap] = {};  // 끝에 구분자 없음
char g_version[kVersionCap] = {};
char g_pdfium[kVersionCap] = {};
char g_last_action[kActionCap] = {};

// ---- 비동기 신호 안전 문자열 도우미 ----
std::size_t append(char* out, std::size_t cap, std::size_t pos, const char* text) noexcept {
  if (text == nullptr) return pos;
  while (*text != '\0' && pos + 1 < cap) out[pos++] = *text++;
  out[pos] = '\0';
  return pos;
}

std::size_t append_number(char* out, std::size_t cap, std::size_t pos, long long value,
                          int base) noexcept {
  char digits[32];
  int n = 0;
  unsigned long long u;
  if (value < 0 && base == 10) {
    if (pos + 1 < cap) out[pos++] = '-';
    u = static_cast<unsigned long long>(-(value + 1)) + 1ULL;
  } else {
    u = static_cast<unsigned long long>(value);
  }
  do {
    const int d = static_cast<int>(u % static_cast<unsigned long long>(base));
    digits[n++] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
    u /= static_cast<unsigned long long>(base);
  } while (u != 0 && n < 31);
  while (n > 0 && pos + 1 < cap) out[pos++] = digits[--n];
  out[pos] = '\0';
  return pos;
}

void copy_bounded(char* dst, std::size_t cap, const char* src) noexcept {
  std::size_t i = 0;
  if (src != nullptr) {
    for (; i + 1 < cap && src[i] != '\0'; ++i) dst[i] = src[i];
  }
  dst[i] = '\0';
}

// "crash-<epoch>.<ext>" 를 폴더 뒤에 붙인다
std::size_t build_file_name(char* out, std::size_t cap, long long epoch, const char* ext) noexcept {
  std::size_t pos = append(out, cap, 0, g_report_dir_utf8);
  pos = append(out, cap, pos, "/crash-");
  pos = append_number(out, cap, pos, epoch, 10);
  pos = append(out, cap, pos, ext);
  return pos;
}

#ifdef _WIN32

LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* ep) {
  if (g_handling.exchange(true)) {  // 필터 안의 예외: 더 할 수 있는 것이 없다
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(0xC000'0000u | 0xDEADu));
    return EXCEPTION_EXECUTE_HANDLER;
  }
  const long long epoch = static_cast<long long>(std::time(nullptr));
  // 1) 기록 파일
  char name[kPathCap + 64];
  build_file_name(name, sizeof name, epoch, ".txt");
  wchar_t wname[kPathCap + 64];
  const int n =
      MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, static_cast<int>(std::size(wname)));
  if (n > 0) {
    HANDLE file = CreateFileW(wname, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      char record[kRecordCap];
      RecordFields fields;
      fields.version = g_version;
      fields.pdfium = g_pdfium;
      fields.is_exception = true;
      fields.signal_or_code = ep != nullptr && ep->ExceptionRecord != nullptr
                                  ? static_cast<long>(ep->ExceptionRecord->ExceptionCode)
                                  : 0;
      fields.last_action = g_last_action;
      fields.epoch = epoch;
      const std::size_t len = format_record(record, sizeof record, fields);
      DWORD written = 0;
      WriteFile(file, record, static_cast<DWORD>(len), &written, nullptr);
      CloseHandle(file);
    }
  }
  // 2) 미니덤프
  build_file_name(name, sizeof name, epoch, ".dmp");
  const int m =
      MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, static_cast<int>(std::size(wname)));
  if (m > 0) {
    HANDLE dump = CreateFileW(wname, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump != INVALID_HANDLE_VALUE) {
      MINIDUMP_EXCEPTION_INFORMATION info;
      info.ThreadId = GetCurrentThreadId();
      info.ExceptionPointers = ep;
      info.ClientPointers = FALSE;
      const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                                   MiniDumpWithThreadInfo);
      MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump, type,
                        ep != nullptr ? &info : nullptr, nullptr, nullptr);
      CloseHandle(dump);
    }
  }
  // 기록·덤프를 남겼으니 WER 창 없이 조용히 끝낸다
  TerminateProcess(GetCurrentProcess(), static_cast<UINT>(0xC000'0000u | 0xDEADu));
  return EXCEPTION_EXECUTE_HANDLER;
}

#else

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGABRT, SIGTRAP, SIGFPE, SIGILL};

void signal_handler(int sig, siginfo_t*, void*) {
  // 먼저 모든 크래시 신호를 기본 처리로 되돌린다 — 핸들러 안에서 또 죽거나 raise 가 다시 들어와도
  // 재진입하지 않는다 (SA_RESETHAND 는 그 신호 하나만 되돌린다)
  for (int s : kSignals) ::signal(s, SIG_DFL);
  if (g_handling.exchange(true)) ::_exit(128 + sig);
  const long long epoch = static_cast<long long>(std::time(nullptr));
  char name[kPathCap + 64];
  build_file_name(name, sizeof name, epoch, ".txt");
  const int fd = ::open(name, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd >= 0) {
    char record[kRecordCap];
    RecordFields fields;
    fields.version = g_version;
    fields.pdfium = g_pdfium;
    fields.is_exception = false;
    fields.signal_or_code = sig;
    fields.last_action = g_last_action;
    fields.epoch = epoch;
    const std::size_t len = format_record(record, sizeof record, fields);
    std::size_t done = 0;
    while (done < len) {
      const ssize_t w = ::write(fd, record + done, len - done);
      if (w <= 0) break;
      done += static_cast<std::size_t>(w);
    }
#if defined(__APPLE__) || defined(__GLIBC__)
    // 시스템 크래시 리포트를 만들지 않으므로(아래) 스택은 여기에 남긴다
    static const char kStackHeader[] = "stack=\n";
    ::write(fd, kStackHeader, sizeof kStackHeader - 1);
    void* frames[64];
    const int n = ::backtrace(frames, 64);
    if (n > 0) ::backtrace_symbols_fd(frames, n, fd);
#endif
    ::close(fd);
  }
  // 조용히 끝낸다: 신호를 다시 올리면 macOS 가 "예기치 않게 종료되었습니다" 창을 띄운다. 기록
  // 파일이 신호·스택을 담고 있으므로 시스템 리포트 없이 끝낸다(종료 코드는 셸 관례 128+신호)
  ::_exit(128 + sig);
}

#endif

}  // namespace

// ---------------------------------------------------------------- 공개 API

bool install(const InstallInfo& info) {
  if (g_installed) return false;
  std::error_code ec;
  std::filesystem::create_directories(info.report_dir, ec);
  if (ec || !std::filesystem::is_directory(info.report_dir, ec)) return false;
  const auto u8 = info.report_dir.u8string();
  const std::string dir(reinterpret_cast<const char*>(u8.data()), u8.size());
  if (dir.size() + 32 >= kPathCap) return false;
  copy_bounded(g_report_dir_utf8, kPathCap, dir.c_str());
  copy_bounded(g_version, kVersionCap, info.app_version.c_str());
  copy_bounded(g_pdfium, kVersionCap, info.pdfium_version.c_str());
#ifdef _WIN32
  SetUnhandledExceptionFilter(unhandled_exception_filter);
#else
  // 스택 오버플로에서도 돌 수 있게 대체 스택
  static std::vector<char> alt_stack(64 * 1024);
  stack_t ss{};
  ss.ss_sp = alt_stack.data();
  ss.ss_size = alt_stack.size();
  ss.ss_flags = 0;
  sigaltstack(&ss, nullptr);
  struct sigaction sa{};
  sa.sa_sigaction = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND | SA_NODEFER;
  for (int sig : kSignals) sigaction(sig, &sa, nullptr);
#endif
  g_installed = true;
  return true;
}

bool installed() noexcept {
  return g_installed;
}

void set_last_action(const char* utf8) noexcept {
  copy_bounded(g_last_action, kActionCap, utf8);
}

const char* signal_name(int signal) noexcept {
#ifndef _WIN32
  switch (signal) {
    case SIGSEGV:
      return "SIGSEGV";
    case SIGBUS:
      return "SIGBUS";
    case SIGABRT:
      return "SIGABRT";
    case SIGTRAP:
      return "SIGTRAP";
    case SIGFPE:
      return "SIGFPE";
    case SIGILL:
      return "SIGILL";
    default:
      break;
  }
#else
  (void)signal;
#endif
  return "SIG?";
}

std::size_t format_record(char* out, std::size_t cap, const RecordFields& fields) noexcept {
  if (out == nullptr || cap == 0) return 0;
  std::size_t pos = append(out, cap, 0, "version=");
  pos = append(out, cap, pos, fields.version);
  pos = append(out, cap, pos, "\npdfium=");
  pos = append(out, cap, pos, fields.pdfium);
  if (fields.is_exception) {
    pos = append(out, cap, pos, "\nexception=0x");
    pos = append_number(out, cap, pos,
                        static_cast<long long>(static_cast<unsigned long>(fields.signal_or_code)),
                        16);
  } else {
    pos = append(out, cap, pos, "\nsignal=");
    pos = append(out, cap, pos, signal_name(static_cast<int>(fields.signal_or_code)));
    pos = append(out, cap, pos, "(");
    pos = append_number(out, cap, pos, fields.signal_or_code, 10);
    pos = append(out, cap, pos, ")");
  }
  pos = append(out, cap, pos, "\nlast_action=");
  pos = append(out, cap, pos, fields.last_action);
  pos = append(out, cap, pos, "\ntime=");
  pos = append_number(out, cap, pos, fields.epoch, 10);
  pos = append(out, cap, pos, "\n");
  return pos;
}

int prune_reports(const std::filesystem::path& dir, int keep) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) return 0;
  // epoch → 파일들 (같은 epoch 의 txt·dmp 는 한 묶음)
  std::map<long long, std::vector<std::filesystem::path>> groups;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) break;
    const std::string name = entry.path().filename().string();
    if (name.rfind("crash-", 0) != 0) continue;
    const std::string ext = entry.path().extension().string();
    if (ext != ".txt" && ext != ".dmp") continue;
    const std::string stem = name.substr(6, name.size() - 6 - ext.size());
    if (stem.empty() || !std::all_of(stem.begin(), stem.end(),
                                     [](unsigned char c) { return std::isdigit(c) != 0; }))
      continue;
    groups[std::stoll(stem)].push_back(entry.path());
  }
  int removed = 0;
  const int total = static_cast<int>(groups.size());
  int index = 0;
  for (const auto& [epoch, files] : groups) {  // 오름차순: 오래된 것부터
    if (total - index <= std::max(0, keep)) break;
    for (const auto& file : files) {
      if (std::filesystem::remove(file, ec)) ++removed;
    }
    ++index;
  }
  return removed;
}

}  // namespace onpdf::app::crash
