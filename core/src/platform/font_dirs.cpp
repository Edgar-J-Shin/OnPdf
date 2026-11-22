#include "onpdf/core/platform/font_dirs.h"

#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <cwchar>
#endif

namespace onpdf::platform {
namespace {

#ifdef _WIN32
std::filesystem::path env_path(const wchar_t* name) {
  const wchar_t* value = _wgetenv(name);
  return value != nullptr && *value != L'\0' ? std::filesystem::path(value)
                                             : std::filesystem::path();
}
#else
std::filesystem::path env_path(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && *value != '\0' ? std::filesystem::path(value)
                                            : std::filesystem::path();
}
#endif

}  // namespace

std::vector<FontDir> system_font_dirs() {
  std::vector<FontDir> dirs;
#ifdef _WIN32
  std::filesystem::path windir = env_path(L"WINDIR");
  if (windir.empty()) windir = "C:\\Windows";
  dirs.push_back({windir / "Fonts", false});
  if (const auto local = env_path(L"LOCALAPPDATA"); !local.empty()) {
    dirs.push_back(
        {local / "Microsoft" / "Windows" / "Fonts", false});  // 사용자 설치 글꼴 (Windows 10+)
  }
#elif defined(__APPLE__)
  dirs.push_back({"/System/Library/Fonts", false});
  dirs.push_back({"/System/Library/Fonts/Supplemental", false});
  dirs.push_back({"/Library/Fonts", false});
  if (const auto home = env_path("HOME"); !home.empty())
    dirs.push_back({home / "Library" / "Fonts", false});
#else
  dirs.push_back({"/usr/share/fonts", true});
  dirs.push_back({"/usr/local/share/fonts", true});
  if (const auto home = env_path("HOME"); !home.empty()) {
    dirs.push_back({home / ".local" / "share" / "fonts", true});
    dirs.push_back({home / ".fonts", true});
  }
#endif
  return dirs;
}

}  // namespace onpdf::platform
