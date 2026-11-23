#pragma once

// 쪽 객체(텍스트·이미지) 값 타입 — PDFium 없음 (#49). 좌표는 쪽 포인트(좌상단 원점, 회전 반영).
// 이미지 디코딩은 앱(Qt)이 하고 core 는 BGRA 픽셀 또는 JPEG 원본 바이트만 받는다 (원칙 1: core 는
// UI 를 모른다).

#include "onpdf/core/engine/text_object.h"
#include "onpdf/core/geometry.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace onpdf::engine {

enum class ObjectKind { kText, kImage };

// 직선(사전 곱셈 아님) 알파 BGRA, stride = width * 4. 투명도는 PDFium 이 SMask 로 넣는다 (#49
// 프로브 A)
struct ImagePixels {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> bgra;
};

// JPEG 원본 바이트: 재압축 없이 /DCTDecode 로 넣는다. 크기는 앱이 헤더에서 읽어 둔다
struct JpegBytes {
  std::vector<std::uint8_t> bytes;
  int width = 0;
  int height = 0;
};

struct ImageSpec {
  std::variant<ImagePixels, JpegBytes> source;
  Rect rect;       // 놓을 자리 (쪽 포인트)
  std::string id;  // 비면 엔진이 만든다 (재실행·복원은 같은 id)
};

// 디코딩 상한 (픽셀 수). 넘으면 거절한다
inline constexpr std::int64_t kMaxImagePixels = 64LL * 1000 * 1000;

// 조회용: 쪽 객체 하나. 텍스트는 OnPdf 마크가 있는 것만, 이미지는 쪽 직속 이미지 전부
// (마크가 없는 기존 이미지는 id = "#obj<쪽 객체 index>", marked = false — 편집 때 mark_object 로
// 마크를 붙인다)
struct PageObjectInfo {
  ObjectKind kind = ObjectKind::kText;
  std::string id;
  bool marked = false;
  int index = -1;       // 쪽 객체 순서 (z 순서: 클수록 위)
  Rect bbox;            // 쪽 포인트
  TextObjectInfo text;  // kind == kText
  int pixel_width = 0;  // kind == kImage
  int pixel_height = 0;
  std::string filter;  // 첫 필터 이름 ("DCTDecode", "FlateDecode" …). 없으면 빈 문자열
};

// 떼어 내 문서가 보관하는 객체 묶음 (삭제·추가 취소). Command 는 이 토큰만 쥔다
using DetachedId = std::uint64_t;

}  // namespace onpdf::engine
