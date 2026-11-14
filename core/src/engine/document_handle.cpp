#include "onpdf/core/engine/document_handle.h"

#include "onpdf/core/engine/pdfium_document.h"
#include "pdfium_worker_impl.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <exception>
#include <utility>

namespace onpdf::engine {

struct DocumentHandle::State {
  std::weak_ptr<PdfiumWorkerImpl> worker;  // 워커가 먼저 소멸하면 lock 이 실패한다
  DocumentId id = 0;
  DocumentInfo info;
  std::atomic<bool> closed{false};

  // 워커에 해제를 맡긴다.
  //  - 워커가 멈췄으면 post 가 cancelled=true 로 inline 호출하고, 워커 쪽 documents 는 이미 비워졌다.
  //  - 워커가 소멸했으면 lock 이 실패하므로 아무것도 하지 않는다.
  void request_close() noexcept {
    if (closed.exchange(true)) return;
    std::shared_ptr<PdfiumWorkerImpl> w = worker.lock();
    if (!w) return;
    const DocumentId doc_id = id;
    try {
      w->post(
          [w, doc_id](bool cancelled) {
            if (!cancelled && w->is_worker_thread()) w->unregister_document(doc_id);
          },
          Priority::kHigh);
    } catch (...) {
      // 할당 실패 등. 워커 종료 시 documents.clear() 가 어차피 해제한다.
    }
  }
};

// DocumentHandle / PdfiumWorker 의 private 멤버에 접근하는 구현 전용 헬퍼.
struct DocumentHandleAccess {
  using State = DocumentHandle::State;

  static std::shared_ptr<PdfiumWorkerImpl> impl_of(PdfiumWorker& worker) { return worker.impl_; }

  // 워커 스레드에서 실행: 문서를 열고 등록한 뒤 핸들을 만든다. 예외는 submit 이 잡는다.
  template <class Opener>
  static Result<DocumentHandle> open_on_worker(const std::shared_ptr<PdfiumWorkerImpl>& impl,
                                               Opener& opener) {
    Result<PdfiumDocument> doc = opener();
    if (!doc) return std::unexpected(doc.error());

    DocumentInfo info;
    info.page_count = doc->page_count();
    info.page_sizes.reserve(static_cast<std::size_t>(std::max(0, info.page_count)));
    for (int i = 0; i < info.page_count; ++i) {
      auto size = doc->page_size(i);
      if (!size) return std::unexpected(size.error());
      info.page_sizes.push_back(*size);
    }

    std::shared_ptr<State> state(new State(), [](State* s) {
      s->request_close();
      delete s;
    });
    state->worker = impl;
    state->info = std::move(info);
    state->id = impl->register_document(std::make_unique<PdfiumDocument>(std::move(*doc)));
    return DocumentHandle(std::move(state));
  }

  // 열기 작업을 워커에 넣고 on_done 을 정확히 한 번 호출한다 (취소·예외 포함).
  template <class Opener>
  static TaskHandle open_with_callback(PdfiumWorker& worker, Opener opener, Priority priority,
                                       DocumentHandle::OpenCallback on_done) {
    assert(on_done && "open 콜백은 비어 있을 수 없다");
    std::shared_ptr<PdfiumWorkerImpl> impl = impl_of(worker);
    return impl->post(
        [impl, opener = std::move(opener), on_done = std::move(on_done)](bool cancelled) mutable {
          Result<DocumentHandle> result = make_error(ErrorCode::kUnknown, "unreachable");
          try {
            if (cancelled) {
              result = make_error(ErrorCode::kCancelled, "open cancelled");
            } else {
              result = open_on_worker(impl, opener);
            }
          } catch (const std::exception& e) {
            result = make_error(ErrorCode::kUnknown, std::string("open threw: ") + e.what());
          } catch (...) {
            result = make_error(ErrorCode::kUnknown, "open threw unknown exception");
          }
          on_done(std::move(result));  // 정확히 한 번
        },
        priority);
  }

  template <class Opener>
  static std::future<Result<DocumentHandle>> open_async(PdfiumWorker& worker, Opener opener,
                                                        Priority priority) {
    auto promise = std::make_shared<std::promise<Result<DocumentHandle>>>();
    auto future = promise->get_future();
    open_with_callback(worker, std::move(opener), priority,
                       [promise](Result<DocumentHandle> r) { promise->set_value(std::move(r)); });
    return future;
  }
};

TaskHandle DocumentHandle::open(PdfiumWorker& worker, std::filesystem::path path,
                                std::string password, Priority priority, OpenCallback on_done) {
  return DocumentHandleAccess::open_with_callback(
      worker,
      [path = std::move(path), password = std::move(password)]() {
        return PdfiumDocument::open(path, password);
      },
      priority, std::move(on_done));
}

TaskHandle DocumentHandle::open_from_memory(PdfiumWorker& worker, std::vector<std::uint8_t> data,
                                            std::string password, Priority priority,
                                            OpenCallback on_done) {
  return DocumentHandleAccess::open_with_callback(
      worker,
      [data = std::move(data), password = std::move(password)]() mutable {
        return PdfiumDocument::open_from_memory(std::move(data), password);
      },
      priority, std::move(on_done));
}

std::future<Result<DocumentHandle>> DocumentHandle::open(PdfiumWorker& worker,
                                                         std::filesystem::path path,
                                                         std::string password, Priority priority) {
  return DocumentHandleAccess::open_async(
      worker,
      [path = std::move(path), password = std::move(password)]() {
        return PdfiumDocument::open(path, password);
      },
      priority);
}

std::future<Result<DocumentHandle>> DocumentHandle::open_from_memory(
    PdfiumWorker& worker, std::vector<std::uint8_t> data, std::string password,
    Priority priority) {
  return DocumentHandleAccess::open_async(
      worker,
      [data = std::move(data), password = std::move(password)]() mutable {
        return PdfiumDocument::open_from_memory(std::move(data), password);
      },
      priority);
}

bool DocumentHandle::valid() const noexcept {
  return state_ && !state_->closed.load(std::memory_order_relaxed);
}

DocumentId DocumentHandle::id() const noexcept { return state_ ? state_->id : 0; }

const DocumentInfo& DocumentHandle::info() const {
  static const DocumentInfo kEmpty;
  return state_ ? state_->info : kEmpty;
}

TaskHandle DocumentHandle::render_page(int page_index, const RenderOptions& options,
                                       Priority priority,
                                       std::function<void(Result<Bitmap>)> on_done) const {
  assert(on_done && "render 콜백은 비어 있을 수 없다");
  if (!state_) {
    on_done(make_error(ErrorCode::kInvalidState, "empty document handle"));
    return TaskHandle{};
  }
  std::shared_ptr<PdfiumWorkerImpl> impl = state_->worker.lock();
  if (!impl) {
    on_done(make_error(ErrorCode::kInvalidState, "worker destroyed"));
    return TaskHandle{};
  }
  std::shared_ptr<State> state = state_;
  RenderOptions opts = options;
  return impl->post(
      [impl, state, page_index, opts, on_done = std::move(on_done)](bool cancelled) {
        Result<Bitmap> result = make_error(ErrorCode::kUnknown, "unreachable");
        try {
          if (cancelled) {
            result = make_error(ErrorCode::kCancelled, "render cancelled");
          } else if (state->closed.load(std::memory_order_relaxed)) {
            result = make_error(ErrorCode::kInvalidState, "document closed");
          } else if (PdfiumDocument* doc = impl->find_document(state->id); doc == nullptr) {
            result = make_error(ErrorCode::kInvalidState, "document not registered");
          } else {
            result = doc->render_page(page_index, opts);
          }
        } catch (const std::exception& e) {
          result = make_error(ErrorCode::kUnknown, std::string("render threw: ") + e.what());
        } catch (...) {
          result = make_error(ErrorCode::kUnknown, "render threw unknown exception");
        }
        on_done(std::move(result));  // 정확히 한 번
      },
      priority);
}

std::future<Result<Bitmap>> DocumentHandle::render_page(int page_index,
                                                        const RenderOptions& options,
                                                        Priority priority) const {
  auto promise = std::make_shared<std::promise<Result<Bitmap>>>();
  auto future = promise->get_future();
  render_page(page_index, options, priority,
              [promise](Result<Bitmap> result) { promise->set_value(std::move(result)); });
  return future;
}

TaskHandle DocumentHandle::render_region(int page_index, const RenderOptions& options,
                                         const Rect& region_px, Priority priority,
                                         std::function<void(Result<Bitmap>)> on_done) const {
  assert(on_done && "render 콜백은 비어 있을 수 없다");
  if (!state_) {
    on_done(make_error(ErrorCode::kInvalidState, "empty document handle"));
    return TaskHandle{};
  }
  std::shared_ptr<PdfiumWorkerImpl> impl = state_->worker.lock();
  if (!impl) {
    on_done(make_error(ErrorCode::kInvalidState, "worker destroyed"));
    return TaskHandle{};
  }
  std::shared_ptr<State> state = state_;
  RenderOptions opts = options;
  return impl->post(
      [impl, state, page_index, opts, region_px, on_done = std::move(on_done)](bool cancelled) {
        Result<Bitmap> result = make_error(ErrorCode::kUnknown, "unreachable");
        try {
          if (cancelled) {
            result = make_error(ErrorCode::kCancelled, "render cancelled");
          } else if (state->closed.load(std::memory_order_relaxed)) {
            result = make_error(ErrorCode::kInvalidState, "document closed");
          } else if (PdfiumDocument* doc = impl->find_document(state->id); doc == nullptr) {
            result = make_error(ErrorCode::kInvalidState, "document not registered");
          } else {
            result = doc->render_region(page_index, opts, region_px);
          }
        } catch (const std::exception& e) {
          result = make_error(ErrorCode::kUnknown, std::string("render threw: ") + e.what());
        } catch (...) {
          result = make_error(ErrorCode::kUnknown, "render threw unknown exception");
        }
        on_done(std::move(result));  // 정확히 한 번
      },
      priority);
}

std::future<Result<Bitmap>> DocumentHandle::render_region(int page_index, const RenderOptions& options,
                                                          const Rect& region_px, Priority priority) const {
  auto promise = std::make_shared<std::promise<Result<Bitmap>>>();
  auto future = promise->get_future();
  render_region(page_index, options, region_px, priority,
                [promise](Result<Bitmap> result) { promise->set_value(std::move(result)); });
  return future;
}

void DocumentHandle::close() noexcept {
  if (state_) state_->request_close();
}

}  // namespace onpdf::engine
