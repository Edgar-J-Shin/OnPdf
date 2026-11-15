#pragma once

// UTF-8 문자열 ↔ std::filesystem::path 변환.
//
// Windows 에서 std::filesystem::path(std::string) 은 UTF-8 이 아니라 시스템 코드 페이지(ACP)로
// 해석하므로 "한글 파일명.pdf" 같은 경로가 깨진다. 내부 문자열은 항상 UTF-8 이므로(CLAUDE.md
// 규칙 4) 경로를 만들 때는 반드시 이 헬퍼를 거친다. u8string 경로는 모든 플랫폼에서 UTF-8 로
// 해석된다.

#include <filesystem>
#include <string>
#include <string_view>

namespace onpdf {

inline std::filesystem::path path_from_utf8(std::string_view utf8) {
  return std::filesystem::path(
      std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

inline std::string path_to_utf8(const std::filesystem::path& path) {
  const std::u8string u8 = path.u8string();
  return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

}  // namespace onpdf
