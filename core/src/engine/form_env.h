#pragma once

// 폼 필 환경 (#51, engine 내부). PDFium 이 FPDF_FORMFILLINFO 의 주소를 보관하므로 구조체를 힙에 고정한다.
// JavaScript 플랫폼(m_pJsPlatform)은 넘기지 않는다 — 원칙 6: JS 실행 비활성.
// 강조색은 앱이 그린다(alpha 0): 타일 캐시·인쇄 결과에 강조가 섞이지 않는다.

#include "onpdf/core/engine/pdfium_handles.h"

#include <fpdf_formfill.h>

#include <memory>

namespace onpdf::engine::detail {

struct FormEnvironment {
  FPDF_FORMFILLINFO info{};
  ScopedFormHandle handle;

  // 폼이 없는 문서(FORMTYPE_NONE)나 초기화 실패면 nullptr
  static std::unique_ptr<FormEnvironment> create(FPDF_DOCUMENT doc);
};

// 쪽의 폼 페이지 뷰 수명 (FORM_OnAfterLoadPage ↔ FORM_OnBeforeClosePage). 쪽(ScopedPage)보다 먼저 소멸해야 한다
class FormPageView {
 public:
  FormPageView(FPDF_PAGE page, FPDF_FORMHANDLE form) noexcept;
  ~FormPageView();
  FormPageView(const FormPageView&) = delete;
  FormPageView& operator=(const FormPageView&) = delete;

 private:
  FPDF_PAGE page_;
  FPDF_FORMHANDLE form_;
};

// 쪽에 위젯 주석이 하나라도 있는가 (없으면 FFLDraw·페이지 뷰를 건너뛴다)
[[nodiscard]] bool page_has_widgets(FPDF_PAGE page);

}  // namespace onpdf::engine::detail
