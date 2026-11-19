#pragma once

// 주석 값 타입 (PDFium 없음). edit/ 의 Command 와 app 의 뷰가 함께 쓴다.
// 좌표: AnnotationInfo·AnnotationSpec 은 "쪽 포인트"(좌상단 원점, 회전 반영), AnnotationSnapshot 은 PDF 사용자 좌표 원시값.

#include "onpdf/core/geometry.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace onpdf::engine {

enum class AnnotationKind { kHighlight, kUnderline, kStrikeOut, kSquiggly, kText, kInk, kSquare, kCircle, kLink, kOther };

struct Color {
  std::uint8_t r = 0, g = 0, b = 0, a = 255;
  friend bool operator==(const Color&, const Color&) = default;
};

// /F 플래그 (PDF 1.7 §12.5.3)
constexpr int kAnnotFlagHidden = 1 << 1;
constexpr int kAnnotFlagPrint = 1 << 2;
constexpr int kAnnotFlagNoZoom = 1 << 3;
constexpr int kAnnotFlagNoRotate = 1 << 4;
constexpr int kAnnotFlagNoView = 1 << 5;

// 조회용 스냅샷 한 항목 (뷰 hit-test·상태 표시). 워커에서 만들고 메인에서 읽는다
struct AnnotationInfo {
  std::string id;                                 // /NM. 없으면 "#<index>" (편집하면 NM 이 부여된다)
  AnnotationKind kind = AnnotationKind::kOther;
  Rect rect;                                      // 쪽 포인트
  Color color;
  bool has_color = false;                         // AP 가 있는 주석은 색을 읽지 못한다
  std::string contents, author, modified;         // UTF-8
  int flags = 0;
  bool editable = false;                          // 마크업·Text·Ink·Square·Circle 만. 링크·위젯·팝업은 편집 대상이 아니다
  float border_width = 0.0f;                      // /Border 폭 (AP 와 무관하게 읽힌다)
  bool has_border = false;
  float opacity = 1.0f;                           // /CA (없으면 1). AP 가 있어도 읽힌다
  bool has_bs = false;                            // /BS 가 있는 주석: 두께는 삭제·재생성으로만 바꿀 수 있다
};

// 선 두께(/Border)가 의미 있는 종류: 펜·사각형·원 (edit 의 Command 와 app 의 메뉴가 같은 기준을 쓴다)
[[nodiscard]] constexpr bool has_border_width(AnnotationKind kind) noexcept {
  return kind == AnnotationKind::kInk || kind == AnnotationKind::kSquare || kind == AnnotationKind::kCircle;
}

// 생성 명세 (Command 가 값으로 보관). 좌표는 쪽 포인트
struct AnnotationSpec {
  AnnotationKind kind = AnnotationKind::kHighlight;
  Color color{255, 255, 0, 255};
  int flags = kAnnotFlagPrint;                    // 새 주석은 /F 가 없어 인쇄되지 않으므로 Print 를 기본으로
  std::vector<Rect> quads;                        // 마크업: 줄별 사각형 (QuadPoints 하나씩)
  Rect rect;                                      // Text(아이콘)·Square·Circle·Ink 의 bbox. 마크업은 quads 합집합으로 계산
  std::vector<std::vector<Point>> ink;            // Ink 스트로크
  double border_width = 0.0;                      // Ink·Square·Circle 선 폭(pt). 0 이면 쓰지 않음
  std::optional<Color> interior;                  // Square·Circle 채움색(/IC). 없으면 채우지 않음. 알파는 무시(color.a 가 /CA)
  std::string contents, author;                   // UTF-8
  std::string id;                                 // /NM. 비어 있으면 엔진이 만든다 (재실행 때 같은 NM 을 쓰도록 Command 가 채운다)
};

// 삭제·색·내용 취소용 원시값 (PDF 사용자 좌표, 좌하단 원점). 같은 NM 으로 다시 만들 수 있다.
// 모양 스트림(AP)은 보관하지 않는다: 스냅샷을 뜰 때 AP 를 지우고 색·내부색·불투명도를 읽어 두며,
// 복원 뒤 PDFium 이 같은 모양을 다시 만든다 (AP 텍스트만 되돌리면 /Resources 가 빠져 하이라이트가 글자를 덮는다)
struct AnnotationSnapshot {
  AnnotationKind kind = AnnotationKind::kOther;
  std::string id;                                 // /NM (스냅샷을 뜰 때 없으면 부여한다)
  std::array<double, 4> rect{};                   // left, bottom, right, top
  std::vector<std::array<float, 8>> quads;        // FS_QUADPOINTSF 순서 (x1 y1 … x4 y4)
  std::vector<std::vector<Point>> ink;            // 사용자 좌표
  Color color;                                    // /C + /CA(a)
  bool has_color = false;
  Color interior;                                 // /IC (Square·Circle 채움). 알파는 color.a 와 같다 (/CA 하나)
  bool has_interior = false;
  float border_width = 0.0f;                      // /Border 만 읽힌다 (/BS /W 는 PDFium API 로 못 읽는다 — 한계)
  bool has_border = false;
  bool has_bs = false;                            // /BS 존재 (복원 때는 쓰지 않는다 — 삭제·재생성으로 두께를 바꾸는 근거)
  int flags = 0;
  std::string contents, author, modified, subject;  // UTF-8
};

}  // namespace onpdf::engine
