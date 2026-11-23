#include "form_env.h"

#include <fpdf_annot.h>

namespace onpdf::engine::detail {

std::unique_ptr<FormEnvironment> FormEnvironment::create(FPDF_DOCUMENT doc) {
  if (doc == nullptr || FPDF_GetFormType(doc) == FORMTYPE_NONE) return nullptr;
  auto env = std::make_unique<FormEnvironment>();
  env->info.version = 1;  // XFA 없음. 콜백은 모두 비워 둔다 (무효화·타이머·JS 없음)
  env->handle.reset(FPDFDOC_InitFormFillEnvironment(doc, &env->info));
  if (!env->handle) return nullptr;
  FPDF_SetFormFieldHighlightAlpha(env->handle.get(), 0);
  return env;
}

FormPageView::FormPageView(FPDF_PAGE page, FPDF_FORMHANDLE form) noexcept
    : page_(page), form_(form) {
  if (page_ != nullptr && form_ != nullptr) FORM_OnAfterLoadPage(page_, form_);
}

FormPageView::~FormPageView() {
  if (page_ != nullptr && form_ != nullptr) FORM_OnBeforeClosePage(page_, form_);
}

bool page_has_widgets(FPDF_PAGE page) {
  const int count = FPDFPage_GetAnnotCount(page);
  for (int i = 0; i < count; ++i) {
    ScopedFPDFAnnotation annot(FPDFPage_GetAnnot(page, i));
    if (annot && FPDFAnnot_GetSubtype(annot.get()) == FPDF_ANNOT_WIDGET) return true;
  }
  return false;
}

}  // namespace onpdf::engine::detail
