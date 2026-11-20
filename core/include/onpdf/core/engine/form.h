#pragma once

// AcroForm 위젯 값 타입 (#51) — PDFium 없음. 좌표는 쪽 포인트(좌상단 원점, 회전 반영).
// 위젯 정체성은 필드 전체 이름 + 컨트롤 index: 주석 추가·삭제로 바뀌는 /Annots 순서를 쓰지 않는다

#include "onpdf/core/geometry.h"

#include <string>
#include <vector>

namespace onpdf::engine {

enum class FormType { kNone, kAcroForm, kXfaFull, kXfaForeground };

enum class FieldType { kUnknown, kPushButton, kCheckBox, kRadio, kComboBox, kListBox, kText, kSignature };

struct FieldRef {
  std::string name;       // 필드 전체 이름 (UTF-8, "부모.자식")
  int control_index = 0;  // 같은 필드의 위젯 순서 (FPDFAnnot_GetFormControlIndex)
  friend bool operator==(const FieldRef&, const FieldRef&) = default;
};

struct FormWidgetInfo {
  FieldRef ref;
  int control_count = 1;  // 같은 필드의 위젯 수 (여러 쪽에 걸칠 수 있다)
  FieldType type = FieldType::kUnknown;
  Rect rect;                      // 쪽 포인트
  std::string value;              // /V (UTF-8). 체크·라디오는 켜진 상태 이름 또는 "Off"
  std::string export_value;       // 체크·라디오 위젯이 켜졌을 때의 값
  bool checked = false;
  int flags = 0;                  // /Ff
  bool read_only = false;
  bool multiline = false;
  bool password = false;
  bool editable_combo = false;
  bool multi_select = false;
  bool comb = false;
  std::vector<std::string> options;  // 콤보·목록 표시 이름
  std::vector<int> selected;         // 선택된 선택지 index
  float font_size = 0.0f;            // /DA 크기 (0 = 자동)
  int max_len = 0;                   // 0 = 제한 없음
  int annot_index = -1;              // 쪽 안 주석 순서 (Tab 이동)
};

[[nodiscard]] constexpr bool is_fillable(FieldType type) noexcept {
  return type == FieldType::kText || type == FieldType::kCheckBox || type == FieldType::kRadio ||
         type == FieldType::kComboBox || type == FieldType::kListBox;
}

}  // namespace onpdf::engine
