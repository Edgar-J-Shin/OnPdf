#pragma once

// OpenType(OTTO) 안의 CFF charset 검사·항등 덮어쓰기 (#73, 내부 — PDFium·qpdf 없음).
//
// hb-subset 은 CID-keyed CFF 의 charset 에 **원래 CID** 를 남긴다(GID 는 0..n-1 로 재배치).
// `CIDFontType0` 로 넣으면 charset 을 보는 뷰어(CoreGraphics·Adobe)는 CID→charset 으로,
// FreeType(PDFium)은 CID=GID 로 글리프를 찾으므로 우리 코드(=새 GID)가 둘 다에서 맞으려면 charset
// 이 항등이어야 한다. charset 은 길이 필드가 없고 뒤 구조는 Top DICT 오프셋으로 찾으므로 같은
// 자리에 더 짧거나 같은 길이의 항등 charset(형식 1: 4B, 형식 2: 5B, 형식 0: 1+2(n-1)B)을 덮어써도
// 구조가 깨지지 않는다(#71 스파이크 검증).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace onpdf::engine::detail {

struct CffCharsetInfo {
  bool cid_keyed = false;          // Top DICT 에 ROS
  unsigned glyphs = 0;             // CharStrings 수
  std::size_t charset_offset = 0;  // 파일(OTF) 기준
  std::size_t charset_len = 0;
  bool identity = false;  // gid i → cid i
};

// OTTO 가 아니거나 구조가 이상하면(범위 밖·사전 정의 charset) nullopt
[[nodiscard]] std::optional<CffCharsetInfo> inspect_cff_charset(std::span<const std::uint8_t> otf);

// cid_keyed 이고 항등이 아니면 같은 자리에 항등 charset 을 쓴다. 이미 항등이면 true. 자리가
// 모자라거나 검사 실패면 false(변경 없음)
[[nodiscard]] bool write_identity_charset(std::vector<std::uint8_t>& otf);

}  // namespace onpdf::engine::detail
