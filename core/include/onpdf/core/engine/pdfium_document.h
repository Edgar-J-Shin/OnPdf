#pragma once

#include "onpdf/core/bitmap.h"
#include "onpdf/core/engine/editable_document.h"
#include "onpdf/core/engine/form.h"
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
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace onpdf::engine {

namespace detail {
struct FormEnvironment;
}

// 열린 PDF 문서. 이동만 가능하며 PDFium 워커 스레드에서만 사용한다.
// 파일 내용은 메모리에 통째로 올린다 (M1 에서 FPDF_LoadCustomDocument 로 전환 예정).
class PdfiumDocument final : public EditableDocument {
 public:
  static Result<PdfiumDocument> open(const std::filesystem::path& path,
                                     std::string_view password = {});
  static Result<PdfiumDocument> open_from_memory(std::vector<std::uint8_t> data,
                                                 std::string_view password = {});

  PdfiumDocument(PdfiumDocument&&) noexcept;
  // 폼 환경 → 문서 → 원본 바이트 순으로 정리한다 (기본 대입은 옛 문서를 폼 환경보다 먼저 닫는다)
  PdfiumDocument& operator=(PdfiumDocument&&) noexcept;
  PdfiumDocument(const PdfiumDocument&) = delete;
  PdfiumDocument& operator=(const PdfiumDocument&) = delete;
  ~PdfiumDocument() override;

  // AcroForm 이 있으면 폼 필 환경을 갖는다 (#51). XFA 는 AcroForm 부분만
  [[nodiscard]] FormType form_type() const noexcept { return form_type_; }
  // 저장 전: AP 가 없는 OnPdf 주석이 있는 쪽을 렌더해 PDFium 이 AP 를 만들게 한다. 렌더한 쪽 수 (#53)
  [[nodiscard]] Result<int> ensure_annotation_appearances() const;

  [[nodiscard]] int page_count() const noexcept override;
  [[nodiscard]] Result<PageSize> page_size(int page_index) const override;

  // ---- 편집 (EditableDocument) ----
  [[nodiscard]] Result<int> page_rotation(int page_index) const override;
  [[nodiscard]] Result<void> set_page_rotation(int page_index, int quarter_turns) override;
  // ---- 구조 편집 (EditableDocument). 스냅샷은 이 문서가 소유하는 별도 PDFium 문서다 ----
  [[nodiscard]] Result<void> delete_pages(const std::vector<int>& pages) override;
  [[nodiscard]] Result<void> move_pages(const std::vector<int>& pages, int dest) override;
  [[nodiscard]] Result<SnapshotId> copy_pages_to_snapshot(const std::vector<int>& pages) override;
  [[nodiscard]] Result<SnapshotId> open_snapshot(const std::filesystem::path& file,
                                                 std::string_view password) override;
  [[nodiscard]] Result<SnapshotInfo> snapshot_info(SnapshotId id) const override;
  [[nodiscard]] Result<void> insert_from_snapshot(SnapshotId id, const std::vector<int>& src_pages,
                                                  int at) override;
  void drop_snapshot(SnapshotId id) noexcept override;
  // ---- 주석 (EditableDocument) ----
  [[nodiscard]] Result<std::vector<AnnotationInfo>> annotations(int page_index) const override;
  [[nodiscard]] Result<std::string> add_annotation(int page_index, const AnnotationSpec& spec) override;
  [[nodiscard]] Result<AnnotationSnapshot> snapshot_annotation(int page_index, std::string_view id) override;
  [[nodiscard]] Result<void> remove_annotation(int page_index, std::string_view id) override;
  [[nodiscard]] Result<void> restore_annotation(int page_index, const AnnotationSnapshot& snapshot) override;
  [[nodiscard]] Result<void> set_annotation_color(int page_index, std::string_view id, Color color) override;
  [[nodiscard]] Result<void> set_annotation_contents(int page_index, std::string_view id,
                                                     std::string_view utf8) override;
  [[nodiscard]] Result<void> set_annotation_border_width(int page_index, std::string_view id, double width) override;
  [[nodiscard]] Result<TextObjectsResult> add_text_objects(int page_index, const TextObjectsSpec& spec) override;
  [[nodiscard]] Result<void> remove_text_objects(int page_index, const std::vector<std::string>& ids) override;
  [[nodiscard]] Result<std::vector<TextObjectInfo>> text_objects(int page_index) const override;
  [[nodiscard]] Result<void> move_page_objects(int page_index, const std::vector<std::string>& ids, Point delta_pt) override;
  [[nodiscard]] Result<std::string> add_image_object(int page_index, const ImageSpec& spec) override;
  [[nodiscard]] Result<std::vector<PageObjectInfo>> page_objects(int page_index) const override;
  [[nodiscard]] Result<std::string> mark_object(int page_index, std::string_view id) override;
  [[nodiscard]] Result<void> set_object_rect(int page_index, std::string_view id, Rect rect_pt) override;
  [[nodiscard]] Result<DetachedId> detach_objects(int page_index, const std::vector<std::string>& ids) override;
  [[nodiscard]] Result<void> reattach_objects(DetachedId id) override;
  void drop_detached(DetachedId id) noexcept override;
  [[nodiscard]] Result<std::vector<FormWidgetInfo>> form_widgets(int page_index) const override;
  [[nodiscard]] Result<std::vector<int>> set_form_text(int page_index, const FieldRef& ref, std::string_view utf8) override;
  [[nodiscard]] Result<std::vector<int>> set_form_checked(int page_index, const FieldRef& ref, bool checked) override;
  [[nodiscard]] Result<std::vector<int>> set_form_selection(int page_index, const FieldRef& ref,
                                                            const std::vector<int>& indices) override;
  // 문서 전체 필드 값 (이름 → 값, 체크·라디오는 상태 이름). 저장 검증용. 폼이 없으면 빈 지도
  [[nodiscard]] Result<std::map<std::string, std::string>> form_values() const;
  [[nodiscard]] int detached_count() const noexcept { return static_cast<int>(detached_.size()); }  // 테스트용
  [[nodiscard]] int snapshot_count() const noexcept { return static_cast<int>(snapshots_.size()); }  // 테스트용
  [[nodiscard]] int font_cache_size() const noexcept { return static_cast<int>(fonts_.size()); }     // 테스트용

  // 폰트 캐시(fonts_)의 글꼴 가운데 어느 쪽의 텍스트 객체도 쓰지 않는 것이 있는가 (취소한 텍스트의 서브셋 — #61).
  // 캐시가 비어 있으면 쪽을 열지 않고 false. 쪽을 못 열면 보수적으로 false (쓰이는지 모르면 지우지 않는다)
  [[nodiscard]] bool has_unreferenced_fonts() const;

  // ---- 기존 텍스트 줄 수정 (#65) ----
  [[nodiscard]] Result<std::vector<TextLineInfo>> text_lines(int page_index) const override;
  [[nodiscard]] Result<bool> line_font_covers(int page_index, std::string_view line_id, std::string_view utf8) const override;
  [[nodiscard]] Result<LineEditResult> remove_text_line(int page_index, std::string_view line_id) override;
  [[nodiscard]] Result<LineEditResult> replace_text_line(int page_index, std::string_view line_id,
                                                         const LineReplacement& replacement) override;
  [[nodiscard]] Result<void> restore_document_state(DocumentStateId id) override;
  void drop_document_state(DocumentStateId id) noexcept override;
  [[nodiscard]] int document_state_count() const noexcept { return static_cast<int>(states_.size()); }  // 테스트용
  void set_state_byte_limit(std::size_t bytes) noexcept { state_byte_limit_ = bytes; }                   // 테스트용
  // 현재 문서를 메모리에 저장 (FPDF_SaveAsCopy, 비증분). 수술·상태 토큰용
  [[nodiscard]] Result<std::vector<std::uint8_t>> save_to_memory() const;

  // ---- 저장 ----
  // 파일에 스트림으로 저장한다 (FPDF_SaveAsCopy). incremental 이면 원본 바이트 뒤에 변경만 덧붙인다(서명 보존).
  // 원자적 교체·검증은 save_document_safely 가 맡는다
  [[nodiscard]] Result<void> save_to(const std::filesystem::path& path, bool incremental) const;
  [[nodiscard]] int signature_count() const;
  // 편집 권한 (보안 처리기 리비전에 따라 비트 해석). 암호화되지 않았거나 소유자 암호로 열었으면 모두 참
  [[nodiscard]] EditPermissions edit_permissions() const;
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
  [[nodiscard]] static EditPermissions permissions_of(FPDF_DOCUMENT doc);
  [[nodiscard]] FPDF_FORMHANDLE form_handle() const noexcept;
  // 구조 편집(쪽 삭제·이동·삽입) 뒤 필드·페이지 뷰를 다시 맞춘다
  void reset_form();
  // 필드 이름 → 위젯이 있는 쪽들 (첫 사용 때 전체 쪽을 훑는다, reset_form 이 비운다)
  [[nodiscard]] std::vector<int> pages_of_field(const std::string& name, int control_count, int page_index) const;
  // 쪽 위젯을 그린다 (렌더 뒤). start·size 는 쪽 전체를 그렸을 때의 장치 좌표
  void draw_form(FPDF_BITMAP bitmap, FPDF_PAGE page, int start_x, int start_y, int size_x, int size_y, int rotate,
                 int flags) const;
  [[nodiscard]] Result<SnapshotId> store_snapshot(std::vector<std::uint8_t> data, ScopedDocument doc);
  // 저장 전처리 (포커스 해제·주석 AP) — save_to 와 save_to_memory 공용
  void prepare_for_save() const;
  [[nodiscard]] Result<TextObjectsResult> add_text_objects_to(FPDF_PAGE page, const TextObjectsSpec& spec);
  // 줄 삭제·교체 공용 (replacement == nullptr 이면 삭제)
  [[nodiscard]] Result<LineEditResult> edit_line(int page_index, std::string_view line_id, const LineReplacement* replacement);
  // 문서를 새 바이트로 다시 연다 (같은 객체 안에서). 실패하면 옛 문서 유지. 폼 환경·폰트 캐시·필드 지도는 재설정
  [[nodiscard]] Result<void> replace_bytes(std::vector<std::uint8_t> bytes);
  // 떼어 둔 객체를 원래 자리에 임시로 되돌려 저장 바이트에 포함시키고(park), 다시 연 뒤 마크 id 로 다시 떼어 낸다(unpark)
  struct ParkedDetached {
    DetachedId id = 0;
    int page = 0;
    std::vector<std::string> mark_ids;
    std::vector<int> indices;  // 원래 쪽 객체 index (되돌릴 때 복원)
  };
  [[nodiscard]] Result<std::vector<ParkedDetached>> park_detached();
  [[nodiscard]] Result<void> unpark_detached(const std::vector<ParkedDetached>& parked);
  // 상태 토큰 저장 (상한을 넘으면 오래된 것부터 폐기)
  DocumentStateId store_state(std::vector<std::uint8_t> bytes);

  // 보조 문서: 삭제 취소용 쪽 사본 또는 삽입 소스 파일. data 가 doc 보다 오래 살아야 한다 (선언 순서)
  struct Snapshot {
    std::vector<std::uint8_t> data;
    ScopedDocument doc;
  };

  // 선언 순서 중요: doc_ 가 data_ 를 참조하므로 doc_ 가 먼저 소멸해야 한다.
  std::vector<std::uint8_t> data_;
  ScopedDocument doc_;
  std::string password_;
  std::unordered_map<SnapshotId, Snapshot> snapshots_;
  SnapshotId next_snapshot_ = 1;
  // 임베딩 폰트 캐시: 서브셋 key → 문서에 로드된 폰트. doc_ 보다 뒤에 선언 → 먼저 닫힌다
  std::unordered_map<std::string, ScopedFont> fonts_;
  // 떼어 낸 쪽 객체 (삭제·추가 취소, #49). doc_ 보다 뒤에 선언 → 문서보다 먼저 Destroy 된다
  struct Detached {
    int page = 0;
    std::vector<std::pair<int, ScopedFPDFPageObject>> objects;  // (원래 쪽 객체 index, 객체) — index 오름차순
    bool foreign = false;  // 마크 없는 기존 객체가 섞여 있다 → 줄 수정의 다시 열기에서 이월할 수 없다 (#65)
  };
  std::unordered_map<DetachedId, Detached> detached_;
  DetachedId next_detached_ = 1;
  // 줄 수정의 상태 토큰 (#65): 수술 전 문서 바이트. 합계 상한을 넘으면 오래된 것부터 폐기한다
  std::unordered_map<DocumentStateId, std::vector<std::uint8_t>> states_;
  std::deque<DocumentStateId> state_order_;
  std::size_t state_bytes_ = 0;
  std::size_t state_byte_limit_ = static_cast<std::size_t>(256) << 20;
  DocumentStateId next_state_ = 1;
  std::unordered_set<std::string> dropped_marks_;  // drop_detached 로 버린 객체의 마크 id — 상태 복원 뒤 되살아나면 지운다
  // 폼 필 환경 (#51). 마지막에 선언 → 가장 먼저 소멸 (FPDFDOC_ExitFormFillEnvironment 는 문서를 닫기 전에)
  FormType form_type_ = FormType::kNone;
  std::unique_ptr<detail::FormEnvironment> form_;
  mutable std::unordered_map<std::string, std::vector<int>> field_pages_;
  mutable bool field_pages_ready_ = false;
};

}  // namespace onpdf::engine
