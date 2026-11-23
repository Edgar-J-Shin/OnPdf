#pragma once

// 기존 텍스트 줄 단위 수정 (#65, M4). PDFium 없음 — edit·app 이 쓴다.
// 다른 프로그램이 만든 텍스트 객체를 줄로 묶어 보여 주고(TextLineInfo), 한 줄을 지우거나 바꾼다.
// 지우기는 qpdf 콘텐츠 스트림 수술(그 줄의 표시 연산자만 삭제)이라 문서를 다시 여는데, 엔진이 수술
// 전 바이트를 상태 토큰(DocumentStateId)으로 보관해 취소는 토큰 복원으로 한다. Command 는 토큰만
// 쥔다(스냅샷 규칙과 같다).

#include "onpdf/core/engine/text_object.h"
#include "onpdf/core/geometry.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace onpdf::engine {

struct TextLineInfo {
  std::string
      id;  // "<p|fN>/<첫 객체 index>/<fnv>" — 같은 문서 바이트에서만 안정 (편집 뒤에는 다시 받는다)
  Rect bbox;               // 쪽 포인트
  std::string text;        // 추출 원문 (앞뒤 공백 제거)
  std::string font_name;   // 첫 객체 BaseFont (서브셋 태그 제거)
  double font_size = 0.0;  // 첫 객체의 쪽 좌표 유효 크기 (pt)
  Color color{0, 0, 0, 255};
  bool font_embedded = false;
  bool font_type3 = false;
  bool mixed_fonts = false;  // 줄 안에 글꼴·크기가 섞여 있다 (수정은 첫 객체 글꼴 하나로 — 안내)
  bool editable = true;      // false 면 reason
  std::string reason;        // "nested-form" | "multi-line-object" | "vertical" | "no-objects"
  int object_count = 0;
  double text_width = 0.0;      // 잉크 폭(앞뒤 공백 제외, 쪽 포인트) (#77)
  double letter_spacing = 0.0;  // 원 줄의 글자당 추가 간격(pt) = (잉크 폭 − 같은 글꼴 자연
                                // 폭)/(글자 수−1). 수정 때 TJ 커닝으로 보존 (#77)
};

// 줄 교체 내용. fallback 이 없으면 원본 글꼴만 쓴다(없는 글자가 있으면 실패). 있으면 원본 글꼴이 못
// 그릴 때 그 스펙(번들 글꼴 서브셋, edit 가 만든 것)으로 넣는다
struct LineReplacement {
  std::string utf8;
  std::optional<TextObjectsSpec> fallback;
  std::optional<double> size;  // 쪽 포인트 유효 크기 (TextLineInfo::font_size 와 같은 단위). 원본
                               // 글꼴 경로는 기준선을 유지한다 (#69)
  std::optional<Color> color;  // RGB 만 (알파는 원본). 대체 경로는 fallback 스펙이 이미 반영한 값
};

using DocumentStateId = std::uint64_t;  // 0 = 없음. 수술 전 문서 바이트를 가리킨다

struct LineEditResult {
  DocumentStateId before = 0;    // 수술을 했으면 수술 전 상태 토큰 (OnPdf 상자 재수정은 0)
  bool used_fallback = false;    // 대체 글꼴로 넣었다
  std::string box;               // 새 텍스트 상자 id (삭제면 빈 문자열)
  std::vector<std::string> ids;  // 새 줄 객체 id
  bool fitted = false;           // 새 객체에 원 줄 자간(TJ 커닝)을 넣었다 (#77)
};

}  // namespace onpdf::engine
