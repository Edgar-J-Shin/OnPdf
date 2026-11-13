#pragma once

// 워커 스레드에 사는 PdfiumDocument 를 가리키는 스레드 안전 핸들.
// UI 는 이 헤더만 보고 PDFium 헤더나 PdfiumDocument 를 보지 않는다.
//
//  - open()/open_from_memory() 는 워커에 열기를 맡기고 future 로 핸들을 돌려준다.
//  - info() 는 열 때 찍은 스냅샷이라 잠금 없이 어디서나 읽을 수 있다. (M2 편집 시 갱신 경로 추가)
//  - render_page() 콜백은 워커 스레드에서 호출된다. UI 는 자기 스레드로 넘겨서 처리한다.
//  - 핸들은 복사 가능하다. 마지막 복사가 사라지거나 close() 가 호출되면 워커에 해제를 맡긴다.
//  - 핸들은 PdfiumWorker 보다 먼저 소멸해야 한다. 워커가 먼저 멈추면 이후 호출은 kInvalidState.

#include "onpdf/core/bitmap.h"
#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/error.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace onpdf::engine {

struct PageSize;
struct RenderOptions;

struct DocumentInfo {
  int page_count = 0;
  std::vector<PageSize> page_sizes;
};

class DocumentHandle {
 public:
  DocumentHandle() = default;  // 빈 핸들

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

  // 콜백 방식. on_done 은 워커 스레드에서 정확히 한 번 호출된다 (취소·실패 포함).
  TaskHandle render_page(int page_index, const RenderOptions& options, Priority priority,
                         std::function<void(Result<Bitmap>)> on_done) const;

  // future 방식 (CLI·테스트용).
  [[nodiscard]] std::future<Result<Bitmap>> render_page(int page_index, const RenderOptions& options,
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
