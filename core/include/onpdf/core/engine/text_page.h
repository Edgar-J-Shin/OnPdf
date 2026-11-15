#pragma once

// FPDF_TEXTPAGE RAII 와 텍스트·링크 추출. 워커 스레드 전용. PdfiumDocument::load_text 가 쓴다.
//
// 좌표 변환: PDFium 의 글자 상자·링크 사각형은 사용자 좌표(좌하단 원점, /Rotate 미적용)다.
// FPDF_PageToDevice 에 장치 크기를 포인트 ×1000 으로 넘겨 0.001pt 정밀도의 "쪽 포인트"(좌상단 원점,
// 회전·CropBox 반영) 를 얻는다. render_region 의 회전 0 매트릭스와 같은 변환이라 화면과 정확히 맞는다.

#include "onpdf/core/engine/pdfium_handles.h"
#include "onpdf/core/error.h"
#include "onpdf/core/geometry.h"
#include "onpdf/core/view/search.h"
#include "onpdf/core/view/text_index.h"

#include <string>
#include <vector>

namespace onpdf::engine {

class TextPage {
 public:
  static Result<TextPage> load(FPDF_PAGE page);
  [[nodiscard]] Result<view::PageTextIndex> extract(FPDF_DOCUMENT doc, FPDF_PAGE page, int page_index) const;
  // PDFium 검색 엔진으로 쿼리(UTF-8)의 모든 위치를 찾는다. 결과는 PDFium 글자 인덱스 범위
  [[nodiscard]] std::vector<view::SourceRange> find(const std::string& query_utf8, view::SearchOptions options) const;
  // PDFium 원본 글자 수 (줄바꿈 문자 포함). 인덱스를 만들지 않고 텍스트 유무만 볼 때
  [[nodiscard]] int raw_char_count() const;

 private:
  explicit TextPage(ScopedTextPage text_page) : text_page_(std::move(text_page)) {}
  ScopedTextPage text_page_;
};

}  // namespace onpdf::engine
