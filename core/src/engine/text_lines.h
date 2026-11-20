#pragma once

// 기존 텍스트 줄 분석 (#65, engine 내부): 쪽의 텍스트 객체(직속 + Form XObject 1단계)를 기준선·방향·간격으로 줄로 묶고
// FPDFText 추출 줄로 검증한다. PDFium 핸들을 다루므로 engine 안에서만 쓴다. 스파이크 #55 의 분석기를 제품화한 것

#include "onpdf/core/engine/text_line.h"

#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <array>
#include <string>
#include <vector>

namespace onpdf::engine::detail {

// 분석한 텍스트 객체 하나 (쪽 사용자 좌표)
struct AnalyzedObject {
  FPDF_PAGEOBJECT handle = nullptr;
  FPDF_PAGEOBJECT parent_form = nullptr;  // 쪽 직속이면 nullptr
  int container = -1;                     // -1 = 쪽 직속, n = 쪽의 n 번째 Form Do (이미지 제외)
  int index = -1;                         // 컨테이너 안 객체 index
  int ordinal = -1;                       // 컨테이너 안 "비어 있지 않은 텍스트 객체" 순번 (qpdf 표시 연산자 순번과 같다)
  int depth = 0;                          // Form 중첩 깊이 (0 = 쪽 직속)
  std::array<double, 6> container_matrix{1, 0, 0, 1, 0, 0};  // 컨테이너 → 쪽 사용자 좌표
  std::array<double, 6> matrix{1, 0, 0, 1, 0, 0};            // 텍스트 행렬 (쪽 사용자 좌표)
  double left = 0, bottom = 0, right = 0, top = 0;           // 경계 (쪽 사용자 좌표)
  bool has_bounds = false;
  FPDF_FONT font = nullptr;
  float size = 0;
  double eff_size = 0;  // 쪽 좌표에서의 글자 높이
  double dir_x = 1, dir_y = 0;
  std::u32string text;
  unsigned r = 0, g = 0, b = 0, a = 255;
  int render_mode = 0;
  std::vector<int> extract_lines;  // 이 객체의 글자가 속한 추출 줄 번호들
};

struct AnalyzedLine {
  std::vector<int> members;  // objects 의 index (진행 방향 순)
  double dir_x = 1, dir_y = 0;
  double eff_size = 0;
  double left = 0, bottom = 0, right = 0, top = 0;  // 쪽 사용자 좌표 bbox
  std::u32string text;                              // 추출 순서로 이어 붙인 원문 (trim)
  int container = -1;
  bool mixed_fonts = false;
  bool multi_line_object = false;  // 어떤 멤버 객체가 두 추출 줄에 걸친다
  bool merged_lines = false;       // 서로 다른 추출 줄을 하나로 묶었다
};

struct PageAnalysis {
  std::vector<AnalyzedObject> objects;
  std::vector<AnalyzedLine> lines;
  int page_text_objects = 0;              // 쪽 직속 비어 있지 않은 텍스트 객체 수
  std::vector<int> form_text_objects;     // Form Do 순번별 (1단계)
  bool has_deep_forms = false;            // 2단계 이상 Form 안에 텍스트가 있다
};

// 쪽 전체 분석. text_page 는 호출자가 연 FPDFText 페이지
PageAnalysis analyze_page(FPDF_PAGE page, FPDF_TEXTPAGE text_page);

// 줄 id ("<p|fN>/<첫 객체 index>/<fnv1a(text)>")
std::string line_id_of(const PageAnalysis& analysis, const AnalyzedLine& line);

// 글꼴이 글자들을 그릴 수 있는가 (.notdef 글리프 서명 비교). 공백은 건너뛴다
bool font_covers(FPDF_FONT font, const std::u32string& chars);

std::string utf32_to_utf8(const std::u32string& s);
std::u32string utf8_to_utf32(std::string_view s);

}  // namespace onpdf::engine::detail
