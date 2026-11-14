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
#include "onpdf/core/view/text_index.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace onpdf::engine {

struct DocumentInfo {
  int page_count = 0;
  std::vector<PageSize> page_sizes;
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

  // 쪽 텍스트 인덱스. 계약은 render_page 와 같다 (콜백은 정확히 한 번).
  TaskHandle load_text(int page_index, Priority priority,
                       std::function<void(Result<view::PageTextIndex>)> on_done) const;
  [[nodiscard]] std::future<Result<view::PageTextIndex>> load_text(int page_index,
                                                                   Priority priority = Priority::kNormal) const;

  // 워커에 문서 해제를 맡긴다. 이후 render_page 는 kInvalidState.
  void close() noexcept;

 private:
  struct State;
  friend struct DocumentHandleAccess;  // .cpp 내부 구현용
  explicit DocumentHandle(std::shared_ptr<State> state) : state_(std::move(state)) {}
  std::shared_ptr<State> state_;
};

}  // namespace onpdf::engine
