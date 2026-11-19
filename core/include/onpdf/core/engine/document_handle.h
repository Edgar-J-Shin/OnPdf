#pragma once

// 워커 스레드에 사는 PdfiumDocument 를 가리키는 스레드 안전 핸들.
// UI 는 이 헤더만 보고 PDFium 헤더나 PdfiumDocument 를 보지 않는다.
//
//  - open()/open_from_memory() 는 워커에 열기를 맡긴다. 콜백 오버로드(UI 용)와 future 오버로드(CLI·테스트)가 있다.
//  - info() 는 열 때 찍은 스냅샷이라 잠금 없이 어디서나 읽을 수 있다. (M2 편집 시 갱신 경로 추가)
//  - render_page() 콜백은 정확히 한 번 호출된다. 보통은 워커 스레드에서 호출되지만,
//    빈 핸들이거나 워커가 이미 멈춘 경우에는 호출 스레드에서 즉시 호출된다.
//    UI 는 두 경우 모두 자기 스레드로 넘겨서 처리한다.
//  - 핸들은 복사 가능하다. 마지막 복사가 사라지거나 close() 가 호출되면 워커에 해제를 맡긴다.
//  - 핸들은 워커보다 오래 살아도 안전하다 (내부적으로 weak 참조). 워커가 멈추거나 소멸한 뒤의
//    render_page 는 각각 kCancelled / kInvalidState 로 끝난다.

#include "onpdf/core/bitmap.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/engine/render_types.h"
#include "onpdf/core/error.h"
#include "onpdf/core/geometry.h"
#include "onpdf/core/engine/editable_document.h"
#include "onpdf/core/engine/save.h"
#include "onpdf/core/view/outline.h"
#include "onpdf/core/view/print_layout.h"
#include "onpdf/core/view/search.h"
#include "onpdf/core/view/text_index.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace onpdf::engine {

// 문서 정보 스냅샷. 워커는 열 때만 채우고(핸들이 만들어지기 전), 그 뒤에는 메인 스레드만 읽고
// apply_change 로 고친다. 워커 작업은 이 값을 읽지 않는다 (잠금 없이 공유하는 근거)
struct DocumentInfo {
  int page_count = 0;
  std::vector<PageSize> page_sizes;
  view::PrintPreferences print;  // 인쇄 권한·문서 인쇄 기본값
  EditPermissions permissions;   // 편집 권한 (종류별). 소유자 암호로 열었거나 암호화되지 않았으면 모두 참
  int signature_count = 0;       // 전자서명 수. 0 보다 크면 저장은 증분 저장
};

class DocumentHandle {
 public:
  DocumentHandle() = default;  // 빈 핸들

  using OpenCallback = std::function<void(Result<DocumentHandle>)>;

  // 콜백 방식. on_done 은 정확히 한 번 호출된다. 보통 워커 스레드에서, 워커가 멈췄으면
  // 호출 스레드에서 즉시 kCancelled 로 호출된다.
  static TaskHandle open(PdfiumWorker& worker, std::filesystem::path path, std::string password,
                         Priority priority, OpenCallback on_done);
  static TaskHandle open_from_memory(PdfiumWorker& worker, std::vector<std::uint8_t> data,
                                     std::string password, Priority priority, OpenCallback on_done);

  // future 방식 (CLI·테스트용).
  static std::future<Result<DocumentHandle>> open(PdfiumWorker& worker, std::filesystem::path path,
                                                  std::string password = {},
                                                  Priority priority = Priority::kHigh);
  static std::future<Result<DocumentHandle>> open_from_memory(PdfiumWorker& worker,
                                                              std::vector<std::uint8_t> data,
                                                              std::string password = {},
                                                              Priority priority = Priority::kHigh);

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] DocumentId id() const noexcept;
  [[nodiscard]] const DocumentInfo& info() const;

  // 콜백 방식. on_done 은 정확히 한 번 호출된다 (취소·실패·예외 포함). 호출 스레드는 위 설명 참고.
  TaskHandle render_page(int page_index, const RenderOptions& options, Priority priority,
                         std::function<void(Result<Bitmap>)> on_done) const;

  // future 방식 (CLI·테스트용).
  [[nodiscard]] std::future<Result<Bitmap>> render_page(int page_index, const RenderOptions& options,
                                                        Priority priority = Priority::kNormal) const;

  // 쪽의 일부(타일)만 렌더. 계약은 render_page 와 같다.
  TaskHandle render_region(int page_index, const RenderOptions& options, const Rect& region_px,
                           Priority priority, std::function<void(Result<Bitmap>)> on_done) const;
  [[nodiscard]] std::future<Result<Bitmap>> render_region(int page_index, const RenderOptions& options,
                                                          const Rect& region_px,
                                                          Priority priority = Priority::kNormal) const;

  // 쪽의 주석 목록 (뷰 hit-test·선택용 스냅샷). 계약은 render_page 와 같다
  TaskHandle load_annotations(int page_index, Priority priority,
                              std::function<void(Result<std::vector<AnnotationInfo>>)> on_done) const;
  [[nodiscard]] std::future<Result<std::vector<AnnotationInfo>>> load_annotations(int page_index,
                                                                               Priority priority = Priority::kLow) const;
  // OnPdf 텍스트 객체 목록 (뷰 선택·이동용 스냅샷, #47). 계약은 load_annotations 와 같다
  TaskHandle load_text_objects(int page_index, Priority priority,
                               std::function<void(Result<std::vector<TextObjectInfo>>)> on_done) const;
  [[nodiscard]] std::future<Result<std::vector<TextObjectInfo>>> load_text_objects(int page_index,
                                                                                 Priority priority = Priority::kLow) const;
  // 쪽 텍스트 인덱스. 계약은 render_page 와 같다 (콜백은 정확히 한 번).
  TaskHandle load_text(int page_index, Priority priority,
                       std::function<void(Result<view::PageTextIndex>)> on_done) const;
  [[nodiscard]] std::future<Result<view::PageTextIndex>> load_text(int page_index,
                                                                   Priority priority = Priority::kNormal) const;

  // 쪽 검색. 계약은 render_page 와 같다.
  TaskHandle search_page(int page_index, std::string query, view::SearchOptions options, Priority priority,
                         std::function<void(Result<view::PageSearchResult>)> on_done) const;
  [[nodiscard]] std::future<Result<view::PageSearchResult>> search_page(int page_index, std::string query,
                                                                        view::SearchOptions options,
                                                                        Priority priority = Priority::kNormal) const;

  // 문서 목차 / 사용자 좌표 → 쪽 포인트. 계약은 render_page 와 같다.
  TaskHandle load_outline(Priority priority, std::function<void(Result<view::Outline>)> on_done) const;
  [[nodiscard]] std::future<Result<view::Outline>> load_outline(Priority priority = Priority::kNormal) const;
  TaskHandle resolve_location(int page_index, Point user_pt, Priority priority,
                              std::function<void(Result<Point>)> on_done) const;
  [[nodiscard]] std::future<Result<Point>> resolve_location(int page_index, Point user_pt,
                                                            Priority priority = Priority::kHigh) const;

  // ---- 편집·저장 ----
  // 문서를 바꾼다 (edit::Command 실행 전용 — 다른 경로로 문서를 바꾸지 않는다). edit 는 워커 스레드에서 실행된다.
  // kHigh: 다음 화면 타일보다 먼저 끝나 새 상태로 그려진다. 계약은 render_page 와 같다
  TaskHandle modify(std::function<Result<ChangeSet>(EditableDocument&)> edit,
                    std::function<void(Result<ChangeSet>)> on_done) const;
  [[nodiscard]] std::future<Result<ChangeSet>> modify(std::function<Result<ChangeSet>(EditableDocument&)> edit) const;
  // 안전 저장 (임시 → 검증 → 교체). 서명 문서면 options.incremental 을 강제로 참으로 한다
  TaskHandle save(std::filesystem::path target, SaveOptions options, std::function<void(Result<SaveResult>)> on_done) const;
  [[nodiscard]] std::future<Result<SaveResult>> save(std::filesystem::path target, SaveOptions options = {}) const;
  // 메인 스레드 전용: 편집 결과로 문서 정보(쪽 크기)를 고친다. 같은 문서의 모든 핸들 사본에 보인다
  void apply_change(const ChangeSet& change);

  // 워커에 문서 해제를 맡긴다. 이후 render_page 는 kInvalidState.
  void close() noexcept;

 private:
  struct State;
  friend struct DocumentHandleAccess;  // .cpp 내부 구현용
  explicit DocumentHandle(std::shared_ptr<State> state) : state_(std::move(state)) {}
  std::shared_ptr<State> state_;
};

}  // namespace onpdf::engine
