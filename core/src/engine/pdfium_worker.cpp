#include "onpdf/core/engine/pdfium_worker.h"

#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/pdfium_library.h"
#include "pdfium_worker_impl.h"

#include <cassert>
#include <cstdio>
#include <exception>

namespace onpdf::engine {

// ---------------------------------------------------------------- Impl

bool PdfiumWorkerImpl::any_pending_locked() const noexcept {
  for (const auto& q : queues) {
    if (!q.empty()) return true;
  }
  return false;
}

// High → Normal → Low 순으로 하나 꺼낸다. 멈추는 중이고 비었으면 false.
bool PdfiumWorkerImpl::pop_next(Entry& out) {
  std::unique_lock lock(mutex);
  cv.wait(lock, [&] { return stopping || any_pending_locked(); });
  for (int p = 2; p >= 0; --p) {
    auto& q = queues[static_cast<std::size_t>(p)];
    if (!q.empty()) {
      out = std::move(q.front());
      q.pop_front();
      return true;
    }
  }
  return false;  // stopping && empty
}

void PdfiumWorkerImpl::run_entry(Entry& entry, bool cancelled) noexcept {
  try {
    entry.task(cancelled);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[onpdf] worker task threw: %s\n", e.what());
  } catch (...) {
    std::fprintf(stderr, "[onpdf] worker task threw an unknown exception\n");
  }
}

void PdfiumWorkerImpl::run_loop(const PdfiumWorker::Options& options) {
  PdfiumLibrary::Options lib_options;
  lib_options.use_system_fonts = options.use_system_fonts;
  PdfiumLibrary library(lib_options);  // 이 스레드에서 생성·소멸

  worker_id.store(std::this_thread::get_id());
  ready.count_down();

  Entry entry;
  while (pop_next(entry)) {
    const bool cancelled = entry.cancelled->load(std::memory_order_relaxed);
    run_entry(entry, cancelled);
    entry = Entry{};
  }

  // stop 이후 post 는 inline 처리되므로 여기서 큐는 비어 있어야 한다. 방어적으로 비운다.
  std::array<std::deque<Entry>, 3> leftovers;
  {
    std::lock_guard lock(mutex);
    leftovers.swap(queues);
  }
  for (auto& q : leftovers) {
    for (auto& e : q) run_entry(e, true);
  }

  documents.clear();  // PdfiumLibrary 보다 먼저 해제
}

void PdfiumWorkerImpl::run(const PdfiumWorker::Options& options) noexcept {
  try {
    run_loop(options);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[onpdf] worker thread terminated by exception: %s\n", e.what());
  } catch (...) {
    std::fprintf(stderr, "[onpdf] worker thread terminated by unknown exception\n");
  }
  // run_loop 가 ready.count_down() 전에 던졌더라도 생성자가 영원히 기다리지 않게 한다.
  if (!ready.try_wait()) ready.count_down();
  worker_id.store(std::thread::id{});
  stop();  // 남은 작업이 있으면 이후 post 가 inline 취소되도록
}

TaskHandle PdfiumWorkerImpl::post(PdfiumWorker::Task task, Priority priority) {
  assert(task && "빈 작업은 넣을 수 없다");
  auto flag = std::make_shared<std::atomic<bool>>(false);
  {
    std::lock_guard lock(mutex);
    if (!stopping) {
      queues[static_cast<std::size_t>(priority)].push_back(Entry{flag, std::move(task)});
      cv.notify_one();
      return TaskHandle(std::move(flag));
    }
  }
  // 이미 멈춘 워커: 호출 스레드에서 취소 통지만 한다.
  flag->store(true, std::memory_order_relaxed);
  Entry entry{flag, std::move(task)};
  run_entry(entry, true);
  return TaskHandle{};
}

void PdfiumWorkerImpl::cancel_all() noexcept {
  std::lock_guard lock(mutex);
  for (auto& q : queues) {
    for (auto& e : q) e.cancelled->store(true, std::memory_order_relaxed);
  }
}

void PdfiumWorkerImpl::stop() noexcept {
  {
    std::lock_guard lock(mutex);
    stopping = true;
    for (auto& q : queues) {
      for (auto& e : q) e.cancelled->store(true, std::memory_order_relaxed);
    }
  }
  cv.notify_all();
}

bool PdfiumWorkerImpl::is_worker_thread() const noexcept {
  return std::this_thread::get_id() == worker_id.load();
}

bool PdfiumWorkerImpl::is_stopped() const noexcept {
  std::lock_guard lock(mutex);
  return stopping;
}

std::size_t PdfiumWorkerImpl::pending_count() const {
  std::lock_guard lock(mutex);
  std::size_t n = 0;
  for (const auto& q : queues) n += q.size();
  return n;
}

DocumentId PdfiumWorkerImpl::register_document(std::unique_ptr<PdfiumDocument> document) {
  assert(is_worker_thread());
  assert(document);
  const DocumentId id = next_document_id++;
  documents.emplace(id, std::move(document));
  return id;
}

PdfiumDocument* PdfiumWorkerImpl::find_document(DocumentId id) const noexcept {
  assert(is_worker_thread());
  auto it = documents.find(id);
  return it == documents.end() ? nullptr : it->second.get();
}

void PdfiumWorkerImpl::unregister_document(DocumentId id) noexcept {
  assert(is_worker_thread());
  documents.erase(id);
}

// ---------------------------------------------------------------- PdfiumWorker

PdfiumWorker::PdfiumWorker(const Options& options) : impl_(std::make_shared<PdfiumWorkerImpl>()) {
  std::shared_ptr<PdfiumWorkerImpl> impl = impl_;
  impl_->thread = std::jthread([impl, options] { impl->run(options); });
  impl_->ready.wait();
}

PdfiumWorker::~PdfiumWorker() {
  assert(!is_worker_thread() && "PdfiumWorker 를 워커 스레드 안에서 소멸시키면 안 된다");
  impl_->stop();
  if (impl_->thread.joinable()) impl_->thread.join();
  // 핸들이 weak_ptr 로 잡고 있을 수 있으므로 Impl 은 마지막 참조가 사라질 때 해제된다.
}

TaskHandle PdfiumWorker::post(Task task, Priority priority) {
  return impl_->post(std::move(task), priority);
}
void PdfiumWorker::cancel_all() noexcept { impl_->cancel_all(); }
void PdfiumWorker::stop() noexcept { impl_->stop(); }
bool PdfiumWorker::is_worker_thread() const noexcept { return impl_->is_worker_thread(); }
bool PdfiumWorker::is_stopped() const noexcept { return impl_->is_stopped(); }
std::size_t PdfiumWorker::pending_count() const { return impl_->pending_count(); }

DocumentId PdfiumWorker::register_document(std::unique_ptr<PdfiumDocument> document) {
  return impl_->register_document(std::move(document));
}
PdfiumDocument* PdfiumWorker::find_document(DocumentId id) const noexcept {
  return impl_->find_document(id);
}
void PdfiumWorker::unregister_document(DocumentId id) noexcept { impl_->unregister_document(id); }

}  // namespace onpdf::engine
