#pragma once

// engine 내부 헬퍼 (공개 include 아님). 링크·목차가 함께 쓰는 대상 해석과 좌표·문자열 변환.
// 모든 함수는 워커 스레드에서만 호출한다.

#include "onpdf/core/engine/pdfium_handles.h"
#include "onpdf/core/geometry.h"
#include "onpdf/core/view/text_index.h"

#include <fpdf_doc.h>
#include <fpdfview.h>

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace onpdf::engine::detail {

// FPDF_PageToDevice 에 장치 크기를 포인트 ×1000 으로 넘겨 0.001pt 정밀도의 "쪽 포인트"(좌상단 원점,
// 회전·CropBox 반영) 를 얻는다. render_region 의 회전 0 매트릭스와 같은 변환
struct PageSpace {
  FPDF_PAGE page;
  int w1000;
  int h1000;

  static PageSpace of(FPDF_PAGE page);
  [[nodiscard]] Point to_pt(double x, double y) const;
  // 사용자 좌표 사각형 → 쪽 포인트 bbox (회전되면 꼭짓점이 뒤바뀌므로 4점 변환)
  [[nodiscard]] Rect to_rect(double left, double right, double bottom, double top) const;
  // 쪽 포인트 → 사용자 좌표 (FPDF_DeviceToPage, 같은 ×1000 장치 공간이라 to_pt 와 정확히 왕복한다)
  [[nodiscard]] Point to_user(Point pt) const;
  // 쪽 포인트 사각형 → 사용자 좌표 {left, bottom, right, top} (4 꼭짓점 변환 뒤 bbox)
  [[nodiscard]] std::array<double, 4> user_rect_of(const Rect& pt) const;
  // 쪽 포인트 사각형 → QuadPoints (Acrobat 순서: 좌상·우상·좌하·우하, 사용자 좌표)
  [[nodiscard]] std::array<float, 8> quad_of(const Rect& pt) const;
  // 쪽 포인트 기준점에 놓인 "바로 선" 객체의 사용자 좌표 행렬 [a b c d e f]:
  // (a,b) = 쪽 포인트 +x(오른쪽) 1pt 가 사용자 좌표에서 가는 방향, (c,d) = 쪽 포인트 −y(위쪽) 1pt 의 방향, (e,f) = 기준점.
  // /Rotate 가 있는 쪽에서도 화면 기준으로 글이 바로 서게 한다 (회전 분기 없음)
  [[nodiscard]] std::array<double, 6> user_matrix_at(Point origin_pt) const;
};

// 대상 → link.page + link.location(쪽 포인트). 위치 변환을 위해 대상 쪽을 연다 (링크: 쪽당 수가 적다)
bool fill_location(FPDF_DOCUMENT doc, FPDF_DEST dest, view::Link& link);
// 대상 → link.page + link.user_location(사용자 좌표, 미변환). 쪽을 열지 않는다 (목차: 항목이 수만 개일 수 있다)
bool fill_target_lazy(FPDF_DOCUMENT doc, FPDF_DEST dest, view::Link& link);

// URI 액션의 URI (원시 바이트)
std::string uri_of(FPDF_DOCUMENT doc, FPDF_ACTION action);

// UTF-8 → UTF-16LE (FPDF_WIDESTRING, NUL 종료). 잘못된 바이트·overlong 은 U+FFFD
std::vector<unsigned short> to_wide(const std::string& utf8);
// UTF-16LE 바이트 → UTF-8. 끝의 NUL 제거, 짝 없는 서러게이트·홀수 바이트는 U+FFFD
std::string utf16le_to_utf8(const unsigned char* data, std::size_t bytes);

}  // namespace onpdf::engine::detail
