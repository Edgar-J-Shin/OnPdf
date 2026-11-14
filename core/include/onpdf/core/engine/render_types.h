#pragma once

// PDFium 에 의존하지 않는 렌더링 관련 값 타입. app 은 이 헤더만 보고 pdfium_document.h 는 보지 않는다.

namespace onpdf::engine {

// 페이지 크기 (PDF 포인트, 1/72 inch)
struct PageSize {
  double width = 0.0;
  double height = 0.0;
};

struct RenderOptions {
  // 포인트당 픽셀 수. 1.0 = 72 DPI, 96 DPI 화면은 96/72.
  float scale = 1.0f;
  // 0, 90, 180, 270 (시계 방향)
  int rotation = 0;
  // 주석(annotation) 렌더링 여부
  bool render_annotations = true;
  // LCD 서브픽셀 텍스트
  bool lcd_text = false;
};

}  // namespace onpdf::engine
