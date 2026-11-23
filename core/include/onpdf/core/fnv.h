#pragma once

// FNV-1a 64 — 저장소 전체의 단일 구현. 같은 알고리즘·표기(16진 16자, 소문자)를 세 곳이 공유한다:
//  - edit 의 서브셋 키(`FontFace::subset` 의 `key`, 바이트)
//  - 엔진의 저장 후처리가 FontFile2 바이트로 OnPdf 글꼴을 찾는 키 (#73 — 값이 어긋나면 변환이 조용히 빠진다)
//  - 줄 id 의 원문 해시(`text_lines.cpp`, UTF-32 를 리틀엔디언 4바이트씩)
// Qt·PDFium 없음.

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

namespace onpdf {

[[nodiscard]] inline std::uint64_t fnv1a64(std::span<const std::uint8_t> bytes) noexcept {
  std::uint64_t h = 14695981039346656037ULL;
  for (const std::uint8_t b : bytes) {
    h ^= b;
    h *= 1099511628211ULL;
  }
  return h;
}

[[nodiscard]] inline std::uint64_t fnv1a64(std::u32string_view text) noexcept {
  std::uint64_t h = 14695981039346656037ULL;
  for (const char32_t c : text) {
    for (int k = 0; k < 4; ++k) {
      h ^= static_cast<std::uint64_t>((c >> (8 * k)) & 0xFF);
      h *= 1099511628211ULL;
    }
  }
  return h;
}

[[nodiscard]] inline std::string fnv1a_hex(std::uint64_t h) {
  char hex[17];
  std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
  return hex;
}

[[nodiscard]] inline std::string fnv1a_hex(std::span<const std::uint8_t> bytes) { return fnv1a_hex(fnv1a64(bytes)); }

}  // namespace onpdf
