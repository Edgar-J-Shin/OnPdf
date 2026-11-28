#include "onpdf/core/engine/document_handle.h"

#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/render_process.h"

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
  // 렌더 자식 프로세스 (#137)
  std::shared_ptr<RenderProcess> render;
  std::atomic<bool> render_fallback{false};

  // 워커에 해제를 맡긴다.
  //  - 워커가 멈췄으면 post 가 cancelled=true 로 inline 호출하고, 워커 쪽 documents 는 이미
  //  비워졌다.
  //  - 워커가 소멸했으면 lock 이 실패하므로 아무것도 하지 않는다.
  void request_close() noexcept {
    if (closed.exchange(true)) return;
    if (render) render->close();  // 자식의 문서도 닫는다 (큐에 넣기만)
    std::shared_ptr<PdfiumWorkerImpl> w = worker.lock();
    if (!w) return;
    const DocumentId doc_id = id;
    try {
      w->post(
          [w, doc_id](bool cancelled) {
            if (!cancelled && w->is_worker_thread()) w->unregister_document(doc_id);
          },
          Priority::kHigh, TaskKind::kOther, -1);
    } catch (...) {
      // 할당 실패 등. 워커 종료 시 documents.clear() 가 어차피 해제한다.
    }
  }
};

// DocumentHandle / PdfiumWorker 의 private 멤버에 접근하는 구현 전용 헬퍼.
struct DocumentHandleAccess {
  using State = DocumentHandle::State;

  static std::shared_ptr<PdfiumWorkerImpl> impl_of(PdfiumWorker& worker) { return worker.impl_; }

  template <class T, class Fn>
  static TaskHandle post_document_task(const DocumentHandle& handle, Priority priority,
                                       TaskKind kind, int page, Fn fn,
                                       std::function<void(Result<T>)> on_done);
  template <class T>
  static std::future<Result<T>> to_future(
      std::function<TaskHandle(std::function<void(Result<T>)>)> start);

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
    // 인쇄 정보는 부가 정보: 실패해도 열기는 성공시키고 기본값(인쇄 허용)을 쓴다
    if (auto print = doc->print_preferences()) info.print = std::move(*print);
    info.permissions = doc->edit_permissions();
    info.signature_count = doc->signature_count();
    info.form_type = doc->form_type();
    info.hazards = doc->render_hazards();  // 열 때 한 번 검사 (#121)

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
        priority, TaskKind::kOpen, -1);
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

std::future<Result<DocumentHandle>> DocumentHandle::open_from_memory(PdfiumWorker& worker,
                                                                     std::vector<std::uint8_t> data,
                                                                     std::string password,
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

DocumentId DocumentHandle::id() const noexcept {
  return state_ ? state_->id : 0;
}

const DocumentInfo& DocumentHandle::info() const {
  static const DocumentInfo kEmpty;
  return state_ ? state_->info : kEmpty;
}

// 문서 작업을 워커에 넣고 콜백을 정확히 한 번 부른다. fn(doc) 는 워커 스레드에서 실행된다.
template <class T, class Fn>
TaskHandle DocumentHandleAccess::post_document_task(const DocumentHandle& handle, Priority priority,
                                                    TaskKind kind, int page, Fn fn,
                                                    std::function<void(Result<T>)> on_done) {
  assert(on_done && "콜백은 비어 있을 수 없다");
  const std::shared_ptr<DocumentHandle::State>& state_ref = handle.state_;
  if (!state_ref) {
    on_done(make_error(ErrorCode::kInvalidState, "empty document handle"));
    return TaskHandle{};
  }
  std::shared_ptr<PdfiumWorkerImpl> impl = state_ref->worker.lock();
  if (!impl) {
    on_done(make_error(ErrorCode::kInvalidState, "worker destroyed"));
    return TaskHandle{};
  }
  std::shared_ptr<DocumentHandle::State> state = state_ref;
  return impl->post(
      [impl, state, fn = std::move(fn), on_done = std::move(on_done)](bool cancelled) mutable {
        Result<T> result = make_error(ErrorCode::kUnknown, "unreachable");
        try {
          if (cancelled) {
            result = make_error(ErrorCode::kCancelled, "task cancelled");
          } else if (state->closed.load(std::memory_order_relaxed)) {
            result = make_error(ErrorCode::kInvalidState, "document closed");
          } else if (PdfiumDocument* doc = impl->find_document(state->id); doc == nullptr) {
            result = make_error(ErrorCode::kInvalidState, "document not registered");
          } else {
            result = fn(*doc);
          }
        } catch (const std::exception& e) {
          result = make_error(ErrorCode::kUnknown, std::string("task threw: ") + e.what());
        } catch (...) {
          result = make_error(ErrorCode::kUnknown, "task threw unknown exception");
        }
        on_done(std::move(result));  // 정확히 한 번
      },
      priority, kind, page);
}

template <class T>
std::future<Result<T>> DocumentHandleAccess::to_future(
    std::function<TaskHandle(std::function<void(Result<T>)>)> start) {
  auto promise = std::make_shared<std::promise<Result<T>>>();
  auto future = promise->get_future();
  start([promise](Result<T> r) { promise->set_value(std::move(r)); });
  return future;
}

TaskHandle DocumentHandle::render_page(int page_index, const RenderOptions& options,
                                       Priority priority,
                                       std::function<void(Result<Bitmap>)> on_done) const {
  if (uses_render_process())
    return state_->render->render_page(page_index, options, priority, std::move(on_done));
  RenderOptions opts = options;
  return DocumentHandleAccess::post_document_task<Bitmap>(
      *this, priority, TaskKind::kRender, page_index,
      [page_index, opts](PdfiumDocument& doc) { return doc.render_page(page_index, opts); },
      std::move(on_done));
}

std::future<Result<Bitmap>> DocumentHandle::render_page(int page_index,
                                                        const RenderOptions& options,
                                                        Priority priority) const {
  return DocumentHandleAccess::to_future<Bitmap>([=, this](std::function<void(Result<Bitmap>)> cb) {
    return render_page(page_index, options, priority, std::move(cb));
  });
}

TaskHandle DocumentHandle::render_region(int page_index, const RenderOptions& options,
                                         const Rect& region_px, Priority priority,
                                         std::function<void(Result<Bitmap>)> on_done) const {
  if (uses_render_process())
    return state_->render->render_region(page_index, options, region_px, priority,
                                         std::move(on_done));
  RenderOptions opts = options;
  return DocumentHandleAccess::post_document_task<Bitmap>(
      *this, priority, TaskKind::kRender, page_index,
      [page_index, opts, region_px](PdfiumDocument& doc) {
        return doc.render_region(page_index, opts, region_px);
      },
      std::move(on_done));
}

std::future<Result<Bitmap>> DocumentHandle::render_region(int page_index,
                                                          const RenderOptions& options,
                                                          const Rect& region_px,
                                                          Priority priority) const {
  return DocumentHandleAccess::to_future<Bitmap>([=, this](std::function<void(Result<Bitmap>)> cb) {
    return render_region(page_index, options, region_px, priority, std::move(cb));
  });
}

TaskHandle DocumentHandle::load_annotations(
    int page_index, Priority priority,
    std::function<void(Result<std::vector<AnnotationInfo>>)> on_done) const {
  return DocumentHandleAccess::post_document_task<std::vector<AnnotationInfo>>(
      *this, priority, TaskKind::kQuery, -1,
      [page_index](PdfiumDocument& doc) { return doc.annotations(page_index); },
      std::move(on_done));
}

std::future<Result<std::vector<AnnotationInfo>>> DocumentHandle::load_annotations(
    int page_index, Priority priority) const {
  return DocumentHandleAccess::to_future<std::vector<AnnotationInfo>>(
      [=, this](std::function<void(Result<std::vector<AnnotationInfo>>)> cb) {
        return load_annotations(page_index, priority, std::move(cb));
      });
}

TaskHandle DocumentHandle::load_text_objects(
    int page_index, Priority priority,
    std::function<void(Result<std::vector<TextObjectInfo>>)> on_done) const {
  return DocumentHandleAccess::post_document_task<std::vector<TextObjectInfo>>(
      *this, priority, TaskKind::kQuery, -1,
      [page_index](PdfiumDocument& doc) { return doc.text_objects(page_index); },
      std::move(on_done));
}

std::future<Result<std::vector<TextObjectInfo>>> DocumentHandle::load_text_objects(
    int page_index, Priority priority) const {
  return DocumentHandleAccess::to_future<std::vector<TextObjectInfo>>(
      [=, this](std::function<void(Result<std::vector<TextObjectInfo>>)> cb) {
        return load_text_objects(page_index, priority, std::move(cb));
      });
}

TaskHandle DocumentHandle::load_text_lines(
    int page_index, Priority priority,
    std::function<void(Result<std::vector<TextLineInfo>>)> on_done) const {
  return DocumentHandleAccess::post_document_task<std::vector<TextLineInfo>>(
      *this, priority, TaskKind::kQuery, -1,
      [page_index](PdfiumDocument& doc) { return doc.text_lines(page_index); }, std::move(on_done));
}

std::future<Result<std::vector<TextLineInfo>>> DocumentHandle::load_text_lines(
    int page_index, Priority priority) const {
  return DocumentHandleAccess::to_future<std::vector<TextLineInfo>>(
      [=, this](std::function<void(Result<std::vector<TextLineInfo>>)> cb) {
        return load_text_lines(page_index, priority, std::move(cb));
      });
}

TaskHandle DocumentHandle::load_page_objects(
    int page_index, Priority priority,
    std::function<void(Result<std::vector<PageObjectInfo>>)> on_done) const {
  return DocumentHandleAccess::post_document_task<std::vector<PageObjectInfo>>(
      *this, priority, TaskKind::kQuery, -1,
      [page_index](PdfiumDocument& doc) { return doc.page_objects(page_index); },
      std::move(on_done));
}

TaskHandle DocumentHandle::load_form_widgets(
    int page_index, Priority priority,
    std::function<void(Result<std::vector<FormWidgetInfo>>)> on_done) const {
  return DocumentHandleAccess::post_document_task<std::vector<FormWidgetInfo>>(
      *this, priority, TaskKind::kQuery, -1,
      [page_index](PdfiumDocument& doc) { return doc.form_widgets(page_index); },
      std::move(on_done));
}

TaskHandle DocumentHandle::load_text(
    int page_index, Priority priority,
    std::function<void(Result<view::PageTextIndex>)> on_done) const {
  return DocumentHandleAccess::post_document_task<view::PageTextIndex>(
      *this, priority, TaskKind::kQuery, -1,
      [page_index](PdfiumDocument& doc) { return doc.load_text(page_index); }, std::move(on_done));
}

std::future<Result<view::PageTextIndex>> DocumentHandle::load_text(int page_index,
                                                                   Priority priority) const {
  return DocumentHandleAccess::to_future<view::PageTextIndex>(
      [=, this](std::function<void(Result<view::PageTextIndex>)> cb) {
        return load_text(page_index, priority, std::move(cb));
      });
}

TaskHandle DocumentHandle::search_page(
    int page_index, std::string query, view::SearchOptions options, Priority priority,
    std::function<void(Result<view::PageSearchResult>)> on_done) const {
  return DocumentHandleAccess::post_document_task<view::PageSearchResult>(
      *this, priority, TaskKind::kQuery, -1,
      [page_index, query = std::move(query), options](PdfiumDocument& doc) {
        return doc.search_page(page_index, query, options);
      },
      std::move(on_done));
}

std::future<Result<view::PageSearchResult>> DocumentHandle::search_page(int page_index,
                                                                        std::string query,
                                                                        view::SearchOptions options,
                                                                        Priority priority) const {
  return DocumentHandleAccess::to_future<view::PageSearchResult>(
      [=, this](std::function<void(Result<view::PageSearchResult>)> cb) {
        return search_page(page_index, query, options, priority, std::move(cb));
      });
}

TaskHandle DocumentHandle::load_outline(Priority priority,
                                        std::function<void(Result<view::Outline>)> on_done) const {
  return DocumentHandleAccess::post_document_task<view::Outline>(
      *this, priority, TaskKind::kQuery, -1, [](PdfiumDocument& doc) { return doc.load_outline(); },
      std::move(on_done));
}

std::future<Result<view::Outline>> DocumentHandle::load_outline(Priority priority) const {
  return DocumentHandleAccess::to_future<view::Outline>(
      [=, this](std::function<void(Result<view::Outline>)> cb) {
        return load_outline(priority, std::move(cb));
      });
}

TaskHandle DocumentHandle::resolve_location(int page_index, Point user_pt, Priority priority,
                                            std::function<void(Result<Point>)> on_done) const {
  return DocumentHandleAccess::post_document_task<Point>(
      *this, priority, TaskKind::kQuery, -1,
      [page_index, user_pt](PdfiumDocument& doc) {
        return doc.resolve_location(page_index, user_pt);
      },
      std::move(on_done));
}

std::future<Result<Point>> DocumentHandle::resolve_location(int page_index, Point user_pt,
                                                            Priority priority) const {
  return DocumentHandleAccess::to_future<Point>([=, this](std::function<void(Result<Point>)> cb) {
    return resolve_location(page_index, user_pt, priority, std::move(cb));
  });
}

TaskHandle DocumentHandle::modify(std::function<Result<ChangeSet>(EditableDocument&)> edit,
                                  std::function<void(Result<ChangeSet>)> on_done) const {
  return DocumentHandleAccess::post_document_task<ChangeSet>(
      *this, Priority::kHigh, TaskKind::kModify, -1,
      [edit = std::move(edit)](PdfiumDocument& doc) { return edit(doc); }, std::move(on_done));
}

std::future<Result<ChangeSet>> DocumentHandle::modify(
    std::function<Result<ChangeSet>(EditableDocument&)> edit) const {
  return DocumentHandleAccess::to_future<ChangeSet>(
      [edit = std::move(edit), this](std::function<void(Result<ChangeSet>)> cb) {
        return modify(edit, std::move(cb));
      });
}

TaskHandle DocumentHandle::save(std::filesystem::path target, SaveOptions options,
                                std::function<void(Result<SaveResult>)> on_done) const {
  if (info().signature_count > 0) options.incremental = true;  // 서명 보존
  return DocumentHandleAccess::post_document_task<SaveResult>(
      *this, Priority::kNormal, TaskKind::kSave, -1,
      [target = std::move(target), options = std::move(options)](PdfiumDocument& doc) {
        return save_document_safely(doc, target, options);
      },
      std::move(on_done));
}

std::future<Result<SaveResult>> DocumentHandle::save(std::filesystem::path target,
                                                     SaveOptions options) const {
  return DocumentHandleAccess::to_future<SaveResult>(
      [target = std::move(target), options = std::move(options),
       this](std::function<void(Result<SaveResult>)> cb) {
        return save(target, options, std::move(cb));
      });
}

void DocumentHandle::apply_change(const ChangeSet& change) {
  if (!state_) return;
  DocumentInfo& info = state_->info;
  if (change.structural) {  // 쪽 수·순서가 바뀌었다: 전체를 새 값으로
    info.page_sizes = change.all_sizes;
    info.page_count = static_cast<int>(change.all_sizes.size());
  } else {
    for (const auto& [page, size] : change.new_sizes) {
      if (page >= 0 && static_cast<std::size_t>(page) < info.page_sizes.size()) {
        info.page_sizes[static_cast<std::size_t>(page)] = size;
      }
    }
  }
  // 렌더 자식 재동기화 (#137): 세대를 먼저 올려 이후 타일 요청을 보류시키고, 워커가 현재 바이트를
  // 만들면 자식에 넘긴다. 바이트를 못 만들면 워커 렌더로 돌아간다
  if (!uses_render_process()) return;
  const std::uint64_t generation = state_->render->begin_resync();
  std::weak_ptr<State> weak = state_;
  DocumentHandleAccess::post_document_task<std::vector<std::uint8_t>>(
      *this, Priority::kHigh, TaskKind::kQuery, -1,
      [](PdfiumDocument& doc) { return doc.save_to_memory(); },
      std::function<void(Result<std::vector<std::uint8_t>>)>(
          [weak, generation](Result<std::vector<std::uint8_t>> bytes) {
            auto state = weak.lock();
            if (!state || !state->render) return;
            if (bytes) {
              state->render->replace(generation, std::move(*bytes));
            } else {
              state->render_fallback.store(true);
              state->render->replace(generation, {});  // 보류를 풀기만 (이후 요청은 워커로)
            }
          }));
}

void DocumentHandle::attach_render_process(std::shared_ptr<RenderProcess> process,
                                           std::filesystem::path path, std::string password) {
  if (!state_ || !process) return;
  state_->render = std::move(process);
  state_->render_fallback.store(state_->render->unavailable());
  state_->render->open(std::move(path), std::move(password));
}

bool DocumentHandle::uses_render_process() const noexcept {
  if (!state_ || !state_->render || state_->closed.load(std::memory_order_relaxed)) return false;
  if (state_->render_fallback.load()) return false;
  if (state_->render->unavailable()) {
    state_->render_fallback.store(true);
    return false;
  }
  return true;
}

void DocumentHandle::close() noexcept {
  if (state_) state_->request_close();
}

}  // namespace onpdf::engine
