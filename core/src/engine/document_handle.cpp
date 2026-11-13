#include "onpdf/core/engine/document_handle.h"

#include "onpdf/core/engine/pdfium_document.h"

#include <algorithm>
#include <atomic>
#include <utility>

namespace onpdf::engine {

struct DocumentHandle::State {
  PdfiumWorker* worker = nullptr;
  DocumentId id = 0;
  DocumentInfo info;
  std::atomic<bool> closed{false};

  // 워커에 해제를 맡긴다. 워커가 이미 멈췄으면 post 가 cancelled=true 로 inline 호출하고,
  // 그때는 워커 쪽 documents 가 이미 비워졌으므로 아무것도 하지 않는다.
  void request_close() noexcept {
    if (closed.exchange(true)) return;
    PdfiumWorker* w = worker;
    const DocumentId doc_id = id;
    w->post(
        [w, doc_id](bool cancelled) {
          if (!cancelled && w->is_worker_thread()) w->unregister_document(doc_id);
        },
        Priority::kHigh);
  }
};

// DocumentHandle 의 private 멤버(State, 생성자)에 접근하는 구현 전용 헬퍼.
struct DocumentHandleAccess {
  using State = DocumentHandle::State;

  // 워커 스레드에서 실행: 문서를 열고 등록한 뒤 핸들 상태를 만든다.
  template <class Opener>
  static Result<std::shared_ptr<State>> open_on_worker(PdfiumWorker& worker, Opener& opener) {
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
    state->worker = &worker;
    state->info = std::move(info);
    state->id = worker.register_document(std::make_unique<PdfiumDocument>(std::move(*doc)));
    return state;
  }

  template <class Opener>
  static std::future<Result<DocumentHandle>> open_async(PdfiumWorker& worker, Opener opener,
                                                        Priority priority) {
    auto promise = std::make_shared<std::promise<Result<DocumentHandle>>>();
    auto future = promise->get_future();
    worker.post(
        [&worker, promise, opener = std::move(opener)](bool cancelled) mutable {
          if (cancelled) {
            promise->set_value(make_error(ErrorCode::kCancelled, "open cancelled"));
            return;
          }
          auto state = open_on_worker(worker, opener);
          if (!state) {
            promise->set_value(std::unexpected(state.error()));
            return;
          }
          promise->set_value(DocumentHandle(std::move(*state)));
        },
        priority);
    return future;
  }
};

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
  if (!state_) {
    on_done(make_error(ErrorCode::kInvalidState, "empty document handle"));
    return TaskHandle{};
  }
  std::shared_ptr<State> state = state_;
  RenderOptions opts = options;
  return state->worker->post(
      [state, page_index, opts, on_done = std::move(on_done)](bool cancelled) {
        if (cancelled) {
          on_done(make_error(ErrorCode::kCancelled, "render cancelled"));
          return;
        }
        if (state->closed.load(std::memory_order_relaxed)) {
          on_done(make_error(ErrorCode::kInvalidState, "document closed"));
          return;
        }
        PdfiumDocument* doc = state->worker->find_document(state->id);
        if (doc == nullptr) {
          on_done(make_error(ErrorCode::kInvalidState, "document not registered"));
          return;
        }
        on_done(doc->render_page(page_index, opts));
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

void DocumentHandle::close() noexcept {
  if (state_) state_->request_close();
}

}  // namespace onpdf::engine
