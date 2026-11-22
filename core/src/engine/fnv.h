#pragma once

// FNV-1a 64 (내부). edit 의 서브셋 키(`FontFace::subset` 의 `key`)와 같은 알고리즘·표기(16진 16자,
// 소문자)라 저장 후처리가 FontFile2 바이트로 OnPdf 가 넣은 글꼴을 찾는 데 쓴다 (#73). 값이 어긋나면
// 변환이 조용히 빠지므로 테스트(`cff_charset_test`)가 edit 의 키와 대조한다

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>

namespace onpdf::engine::detail {

[[nodiscard]] inline std::uint64_t fnv1a64(std::span<const std::uint8_t> bytes) noexcept {
  std::uint64_t h = 14695981039346656037ULL;
  for (const std::uint8_t b : bytes) {
    h ^= b;
    h *= 1099511628211ULL;
  }
  return h;
}

[[nodiscard]] inline std::string fnv1a_hex(std::span<const std::uint8_t> bytes) {
  char hex[17];
  std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(fnv1a64(bytes)));
  return hex;
}

}  // namespace onpdf::engine::detail
