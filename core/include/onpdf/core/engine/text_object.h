#pragma once

// 텍스트 객체(쪽 콘텐츠) 값 타입 — PDFium 없음. edit/ 의 AddTextCommand 가 만들고 엔진이 쓴다.
// 좌표는 쪽 포인트(좌상단 원점, 회전 반영). 글자 배치·서브셋·ToUnicode 는 edit(HarfBuzz)가 만들고,
// 엔진은 받은 바이트를 FPDFText_LoadCidType2Font 로 넣는다 (#43: PDFium 자동 ToUnicode·전체 임베딩 금지).

#include "onpdf/core/engine/annotation.h"  // Color
#include "onpdf/core/geometry.h"

#include <cstdint>
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
};

struct TextObjectsSpec {
  EmbeddedFontSpec font;
  std::vector<TextLineSpec> lines;
  std::vector<std::string> ids;  // 줄별 객체 id. 비어 있으면 엔진이 만든다 (재실행은 같은 id 를 준다)
};

// 조회용: OnPdf 가 만든(마크가 있는) 텍스트 객체
struct TextObjectInfo {
  std::string id;  // 마크 "OnPdf" 의 id 파라미터 ("onpdf-text-<16진 16자>")
  Rect bbox;       // 쪽 포인트
};

}  // namespace onpdf::engine
