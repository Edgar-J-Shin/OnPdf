#pragma once

#include "onpdf/core/bitmap.h"
#include "onpdf/core/engine/editable_document.h"
#include "onpdf/core/engine/pdfium_handles.h"
#include "onpdf/core/engine/render_types.h"
#include "onpdf/core/error.h"
#include "onpdf/core/geometry.h"
#include "onpdf/core/view/outline.h"
#include "onpdf/core/view/print_layout.h"
#include "onpdf/core/view/search.h"
#include "onpdf/core/view/text_index.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace onpdf::engine {

// 열린 PDF 문서. 이동만 가능하며 PDFium 워커 스레드에서만 사용한다.
// 파일 내용은 메모리에 통째로 올린다 (M1 에서 FPDF_LoadCustomDocument 로 전환 예정).
class PdfiumDocument final : public EditableDocument {
 public:
  static Result<PdfiumDocument> open(const std::filesystem::path& path,
                                     std::string_view password = {});
  static Result<PdfiumDocument> open_from_memory(std::vector<std::uint8_t> data,
                                                 std::string_view password = {});

  PdfiumDocument(PdfiumDocument&&) noexcept = default;
  PdfiumDocument& operator=(PdfiumDocument&&) noexcept = default;
  PdfiumDocument(const PdfiumDocument&) = delete;
  PdfiumDocument& operator=(const PdfiumDocument&) = delete;
  ~PdfiumDocument() override = default;

  [[nodiscard]] int page_count() const noexcept override;
  [[nodiscard]] Result<PageSize> page_size(int page_index) const override;

  // ---- 편집 (EditableDocument) ----
  [[nodiscard]] Result<int> page_rotation(int page_index) const override;
  [[nodiscard]] Result<void> set_page_rotation(int page_index, int quarter_turns) override;

  // ---- 저장 ----
  // 파일에 스트림으로 저장한다 (FPDF_SaveAsCopy). incremental 이면 원본 바이트 뒤에 변경만 덧붙인다(서명 보존).
  // 원자적 교체·검증은 save_document_safely 가 맡는다
  [[nodiscard]] Result<void> save_to(const std::filesystem::path& path, bool incremental) const;
  [[nodiscard]] int signature_count() const;
  // 수정 권한(권한 비트 4). 암호화되지 않았거나 소유자 암호로 열었으면 참
  [[nodiscard]] bool can_modify() const;
  // 열 때 쓴 암호 (저장 검증에서 같은 암호로 다시 열기 위해)
  [[nodiscard]] const std::string& password() const noexcept { return password_; }
  // 열 때 읽은 원본 바이트 (증분 저장 검증: 새 파일 앞부분과 같아야 한다)
  [[nodiscard]] const std::vector<std::uint8_t>& original_bytes() const noexcept { return data_; }
  [[nodiscard]] Result<Bitmap> render_page(int page_index, const RenderOptions& options) const;
  // 쪽을 options.scale 로 렌더한 픽셀 평면에서 region_px 사각형만 그린다 (타일 렌더).
  // options.rotation 은 무시한다 — 쪽 크기(page_size)가 이미 /Rotate 를 반영하므로 회전 0 기준.
  [[nodiscard]] Result<Bitmap> render_region(int page_index, const RenderOptions& options,
                                             const Rect& region_px) const;
  // 쪽의 텍스트 인덱스(글자·줄·링크). 좌표는 쪽 포인트(좌상단 원점, 회전 반영)
  [[nodiscard]] Result<view::PageTextIndex> load_text(int page_index) const;
  // 문서 목차. 순환·깊이(32)·노드 수(10만) 제한. 대상 위치는 사용자 좌표(user_location)로만 보관
  [[nodiscard]] Result<view::Outline> load_outline() const;
  // 인쇄 권한(FPDF_GetDocPermissions)과 문서가 정한 인쇄 기본값(/ViewerPreferences)
  [[nodiscard]] Result<view::PrintPreferences> print_preferences() const;
  // 쪽의 사용자 좌표 → 쪽 포인트(좌상단 원점, 회전 반영)
  [[nodiscard]] Result<Point> resolve_location(int page_index, Point user_pt) const;
  // 쪽 검색: 텍스트 페이지를 한 번 열어 인덱스 추출 + PDFium 검색 + 인덱스 범위 매핑
  [[nodiscard]] Result<view::PageSearchResult> search_page(int page_index, const std::string& query_utf8,
                                                           view::SearchOptions options) const;

 private:
  PdfiumDocument(std::vector<std::uint8_t> data, ScopedDocument doc);

  Result<ScopedPage> load_page(int page_index) const;

  // 선언 순서 중요: doc_ 가 data_ 를 참조하므로 doc_ 가 먼저 소멸해야 한다.
  std::vector<std::uint8_t> data_;
  ScopedDocument doc_;
  std::string password_;
};

}  // namespace onpdf::engine
