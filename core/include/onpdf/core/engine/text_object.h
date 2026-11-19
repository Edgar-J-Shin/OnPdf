#pragma once

// 텍스트 객체(쪽 콘텐츠) 값 타입 — PDFium 없음. edit/ 의 AddTextCommand 가 만들고 엔진이 쓴다.
// 좌표는 쪽 포인트(좌상단 원점, 회전 반영). 글자 배치·서브셋·ToUnicode 는 edit(HarfBuzz)가 만들고,
// 엔진은 받은 바이트를 FPDFText_LoadCidType2Font 로 넣는다 (#43: PDFium 자동 ToUnicode·전체 임베딩 금지).

#include "onpdf/core/engine/annotation.h"  // Color
#include "onpdf/core/geometry.h"

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace onpdf::engine {

// 임베딩할 서브셋 폰트. key 는 ttf 바이트의 해시 — 문서가 같은 key 의 FPDF_FONT 를 재사용한다(두 번 임베딩 방지)
struct EmbeddedFontSpec {
  std::string key;
  std::vector<std::uint8_t> ttf;           // 정적 TrueType(서브셋, GID 재배치됨)
  std::string to_unicode_cmap;             // CID → 유니코드 (bfchar CMap 텍스트)
  std::vector<std::uint8_t> cid_to_gid;    // 2바이트 big-endian × 글리프 수 (Identity)
};

// 텍스트 객체 하나 = 한 줄. cids 는 서브셋 폰트의 GID(= CID, Identity-H)
struct TextLineSpec {
  std::vector<std::uint32_t> cids;
  Point origin;             // 기준선 왼쪽 끝 (쪽 포인트)
  double font_size = 24.0;  // pt
  Color color{0, 0, 0, 255};
  std::string text;         // 줄 원문(UTF-8) — 마크 블롭 "text" 로 보관해 수정 때 되읽는다 (#47). 비면 쓰지 않는다
};

struct TextObjectsSpec {
  EmbeddedFontSpec font;
  std::vector<TextLineSpec> lines;
  std::vector<std::string> ids;  // 줄별 객체 id. 비어 있으면 엔진이 만든다 (재실행은 같은 id 를 준다)
  std::string box;               // 상자 id("onpdf-box-<16진 16자>"): 줄들을 묶는다. 비면 엔진이 만든다 (#47)
  std::string box_text;          // 상자 원문 전체(빈 줄 포함, '\n' 구분) — 첫 줄 객체의 블롭 "box_text". 빈 줄은 객체가 없어 줄 원문만으로는 복원이 안 된다
  bool write_box = true;         // 테스트용: false 면 box·box_text 마크를 쓰지 않는다 (#47 이전 파일 흉내)
};

struct TextObjectsResult {
  std::vector<std::string> ids;
  std::string box;
};

// 텍스트 객체·상자 id ("onpdf-text-<16진 16자>" / "onpdf-box-…"). 엔진과 edit 가 같은 형식을 쓴다
[[nodiscard]] inline std::string make_onpdf_id(const char* prefix) {
  std::random_device random;
  const std::uint64_t nonce = (static_cast<std::uint64_t>(random()) << 32) ^ static_cast<std::uint64_t>(random());
  char hex[17];
  std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(nonce));
  return std::string(prefix) + hex;
}
[[nodiscard]] inline std::string make_text_object_id() { return make_onpdf_id("onpdf-text-"); }
[[nodiscard]] inline std::string make_text_box_id() { return make_onpdf_id("onpdf-box-"); }

// 조회용: OnPdf 가 만든(마크가 있는) 텍스트 객체
struct TextObjectInfo {
  std::string id;       // 마크 "OnPdf" 의 id 파라미터 ("onpdf-text-<16진 16자>")
  std::string box;      // 상자 id (마크가 없는 옛 객체는 id 와 같다)
  std::string text;     // 줄 원문(마크 블롭). 없으면 ToUnicode 추출로 대체 (공유 글리프 한계)
  std::string box_text; // 상자 원문 전체 (첫 줄 객체에만, 빈 줄 포함). 없으면 호출자가 줄 원문을 잇는다
  double font_size = 0.0;
  Color color{0, 0, 0, 255};
  Point baseline_origin;  // 기준선 왼쪽 끝 (쪽 포인트)
  Rect bbox;              // 쪽 포인트
  int index = -1;         // 쪽 객체 순서 (z 순서)
};

}  // namespace onpdf::engine
