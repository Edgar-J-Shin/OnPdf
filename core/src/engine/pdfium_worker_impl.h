#pragma once

// PdfiumWorker 의 내부 상태. core/src/engine 안에서만 include 한다.
// DocumentHandle 은 이 객체를 weak_ptr 로 잡아, 워커가 먼저 소멸해도 안전하게 실패한다.

#include "onpdf/core/engine/pdfium_worker.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <latch>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace onpdf::engine {

struct PdfiumWorkerImpl {
  struct Entry {
    std::shared_ptr<std::atomic<bool>> cancelled;
    PdfiumWorker::Task task;
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

  TaskHandle post(PdfiumWorker::Task task, Priority priority);
  void cancel_all() noexcept;
  void stop() noexcept;
  [[nodiscard]] bool is_worker_thread() const noexcept;
  [[nodiscard]] bool is_stopped() const noexcept;
  [[nodiscard]] std::size_t pending_count() const;

  DocumentId register_document(std::unique_ptr<PdfiumDocument> document);
  [[nodiscard]] PdfiumDocument* find_document(DocumentId id) const noexcept;
  void unregister_document(DocumentId id) noexcept;

  void run(const PdfiumWorker::Options& options) noexcept;

 private:
  bool any_pending_locked() const noexcept;
  bool pop_next(Entry& out);
  static void run_entry(Entry& entry, bool cancelled) noexcept;
  void run_loop(const PdfiumWorker::Options& options);
};

}  // namespace onpdf::engine
