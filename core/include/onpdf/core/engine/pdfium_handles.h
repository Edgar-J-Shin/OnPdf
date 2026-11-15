#pragma once

// PDFium 핸들의 RAII 래퍼.
// PDFium 이 제공하는 cpp/fpdf_scopers.h 의 unique_ptr 별칭을 onpdf 이름공간으로 가져온다.
// core/engine 밖에서는 raw FPDF_* 핸들을 다루지 않는다.

#include <fpdf_scopers.h>
#include <fpdfview.h>

namespace onpdf::engine {

using ScopedDocument = ScopedFPDFDocument;
using ScopedPage = ScopedFPDFPage;
using ScopedTextPage = ScopedFPDFTextPage;
using ScopedBitmap = ScopedFPDFBitmap;
using ScopedFormHandle = ScopedFPDFFormHandle;
using ScopedFont = ScopedFPDFFont;
using ScopedAnnotation = ScopedFPDFAnnotation;

}  // namespace onpdf::engine
