#include "onpdf/core/engine/pdfium_worker.h"

#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/pdfium_library.h"

#include <array>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <latch>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace onpdf::engine {

struct PdfiumWorker::Impl {
  struct Entry {
    std::shared_ptr<std::atomic<bool>> cancelled;
    Task task;
  };

  // ---- 여러 스레드가 접근: mutex 로 보호 ----
  mutable std::mutex mutex;
  std::condition_variable cv;
  std::array<std::deque<Entry>, 3> queues;  // index = Priority
  bool stopping = false;

  // ---- 워커 스레드 전용 ----
  std::unordered_map<DocumentId, std::unique_ptr<PdfiumDocument>> documents;
  std::uint64_t next_document_id = 1;

  std::atomic<std::thread::id> worker_id{};
  std::latch ready{1};
  std::jthread thread;

  bool any_pending_locked() const noexcept {
    for (const auto& q : queues) {
      if (!q.empty()) return true;
    }
    return false;
  }

  // High → Normal → Low 순으로 하나 꺼낸다. 멈추는 중이고 비었으면 false.
  bool pop_next(Entry& out) {
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

  static void run_entry(Entry& entry, bool cancelled) noexcept {
    try {
      entry.task(cancelled);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[onpdf] worker task threw: %s\n", e.what());
    } catch (...) {
      std::fprintf(stderr, "[onpdf] worker task threw an unknown exception\n");
    }
  }

  void run(const PdfiumWorker::Options& options) {
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

    // stop 이후 들어온 작업은 없다(post 가 inline 처리). 남은 작업은 전부 취소 통지.
    // pop_next 가 false 를 돌려준 시점에 큐는 비어 있지만, 방어적으로 한 번 더 비운다.
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
};

PdfiumWorker::PdfiumWorker(const Options& options) : impl_(std::make_unique<Impl>()) {
  impl_->thread = std::jthread([this, options] { impl_->run(options); });
  impl_->ready.wait();
}

PdfiumWorker::~PdfiumWorker() {
  stop();
  if (impl_->thread.joinable()) impl_->thread.join();
}

TaskHandle PdfiumWorker::post(Task task, Priority priority) {
  assert(task && "빈 작업은 넣을 수 없다");
  auto flag = std::make_shared<std::atomic<bool>>(false);
  {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->stopping) {
      impl_->queues[static_cast<std::size_t>(priority)].push_back(
          Impl::Entry{flag, std::move(task)});
      impl_->cv.notify_one();
      return TaskHandle(std::move(flag));
    }
  }
  // 이미 멈춘 워커: 호출 스레드에서 취소 통지만 한다.
  flag->store(true, std::memory_order_relaxed);
  Impl::Entry entry{flag, std::move(task)};
  Impl::run_entry(entry, true);
  return TaskHandle{};
}

void PdfiumWorker::cancel_all() noexcept {
  std::lock_guard lock(impl_->mutex);
  for (auto& q : impl_->queues) {
    for (auto& e : q) e.cancelled->store(true, std::memory_order_relaxed);
  }
}

void PdfiumWorker::stop() noexcept {
  {
    std::lock_guard lock(impl_->mutex);
    impl_->stopping = true;
    for (auto& q : impl_->queues) {
      for (auto& e : q) e.cancelled->store(true, std::memory_order_relaxed);
    }
  }
  impl_->cv.notify_all();
}

bool PdfiumWorker::is_worker_thread() const noexcept {
  return std::this_thread::get_id() == impl_->worker_id.load();
}

bool PdfiumWorker::is_stopped() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->stopping;
}

std::size_t PdfiumWorker::pending_count() const {
  std::lock_guard lock(impl_->mutex);
  std::size_t n = 0;
  for (const auto& q : impl_->queues) n += q.size();
  return n;
}

DocumentId PdfiumWorker::register_document(std::unique_ptr<PdfiumDocument> document) {
  assert(is_worker_thread());
  assert(document);
  const DocumentId id = impl_->next_document_id++;
  impl_->documents.emplace(id, std::move(document));
  return id;
}

PdfiumDocument* PdfiumWorker::find_document(DocumentId id) const noexcept {
  assert(is_worker_thread());
  auto it = impl_->documents.find(id);
  return it == impl_->documents.end() ? nullptr : it->second.get();
}

void PdfiumWorker::unregister_document(DocumentId id) noexcept {
  assert(is_worker_thread());
  impl_->documents.erase(id);
}

}  // namespace onpdf::engine
