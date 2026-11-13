#pragma once

// PDFium 전용 워커 스레드와 우선순위 작업 큐.
//
// PDFium 은 스레드 안전하지 않다. PdfiumWorker 는 스레드 하나를 띄워 그 안에서 PdfiumLibrary 를
// 생성하고, 모든 PDFium 호출을 이 스레드에서만 실행한 뒤, 종료 시 같은 스레드에서 해제한다.
// 다른 스레드는 post()/submit() 으로 작업을 넣고 콜백(워커 스레드에서 호출) 또는 future 로 결과를 받는다.
//
// 규칙
//  - 앱은 PdfiumWorker 하나만 만든다. CLI·테스트처럼 단일 스레드인 경우에는 PdfiumLibrary 를
//    직접 만들어 써도 되지만, 한 프로세스에서 둘을 섞지 않는다.
//  - 콜백은 보통 워커 스레드에서 호출된다. 단, 워커가 이미 멈춘 뒤 post 하면 호출 스레드에서
//    cancelled=true 로 즉시 호출된다. UI 는 두 경우 모두 자기 스레드로 넘긴 뒤 처리한다.
//  - 취소는 아직 시작하지 않은 작업에만 적용된다. 취소된 작업은 task(true) 로 통지된다.
//  - 워커를 워커 스레드 안에서 소멸시키지 않는다 (자기 join).

#include "onpdf/core/error.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <type_traits>
#include <utility>

namespace onpdf::engine {

class PdfiumDocument;
class PdfiumLibrary;
struct PdfiumWorkerImpl;

enum class Priority { kLow = 0, kNormal = 1, kHigh = 2 };

using DocumentId = std::uint64_t;  // 0 = 없음

// 큐에 넣은 작업의 취소 토큰. 복사 가능하며 작업이 끝난 뒤에도 안전하다.
class TaskHandle {
 public:
  TaskHandle() = default;

  // 아직 시작하지 않았으면 취소한다. 이미 실행 중이거나 끝났으면 아무 효과가 없다.
  void cancel() noexcept {
    if (cancelled_) cancelled_->store(true, std::memory_order_relaxed);
  }
  [[nodiscard]] bool is_cancelled() const noexcept {
    return cancelled_ && cancelled_->load(std::memory_order_relaxed);
  }
  [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(cancelled_); }

 private:
  friend struct PdfiumWorkerImpl;
  explicit TaskHandle(std::shared_ptr<std::atomic<bool>> flag) : cancelled_(std::move(flag)) {}
  std::shared_ptr<std::atomic<bool>> cancelled_;
};

class PdfiumWorker {
 public:
  // 워커 스레드에서 실행된다. cancelled == true 면 PDFium 을 쓰지 말고 통지·정리만 한다.
  using Task = std::function<void(bool cancelled)>;

  struct Options {
    bool use_system_fonts = true;
  };

  PdfiumWorker() : PdfiumWorker(Options{}) {}
  explicit PdfiumWorker(const Options& options);
  // 대기 작업 전부 취소 통지 → 열린 문서 해제 → PDFium 해제 → join
  ~PdfiumWorker();

  PdfiumWorker(const PdfiumWorker&) = delete;
  PdfiumWorker& operator=(const PdfiumWorker&) = delete;
  PdfiumWorker(PdfiumWorker&&) = delete;
  PdfiumWorker& operator=(PdfiumWorker&&) = delete;

  // 작업을 큐에 넣는다. 워커가 이미 멈췄으면 호출 스레드에서 즉시 task(true) 를 실행하고 빈 핸들을 돌려준다.
  TaskHandle post(Task task, Priority priority = Priority::kNormal);

  // fn 을 워커에서 실행하고 결과를 future 로 받는다.
  // fn 은 Result<T> 또는 T 를 돌려줄 수 있다. 취소 → kCancelled, 예외 → kUnknown.
  template <class F>
  auto submit(F&& fn, Priority priority = Priority::kNormal);

  // 대기 중인 작업을 전부 취소한다. 실행 중인 작업은 끝까지 간다.
  void cancel_all() noexcept;

  // 새 작업을 받지 않고 대기 작업을 취소한다. 소멸자가 호출하며, 테스트에서 명시적으로 부를 수 있다.
  void stop() noexcept;

  [[nodiscard]] bool is_worker_thread() const noexcept;
  [[nodiscard]] bool is_stopped() const noexcept;
  [[nodiscard]] std::size_t pending_count() const;

  // ---- 아래는 워커 스레드에서만 호출한다 (DocumentHandle 구현이 사용) ----
  DocumentId register_document(std::unique_ptr<PdfiumDocument> document);
  [[nodiscard]] PdfiumDocument* find_document(DocumentId id) const noexcept;
  void unregister_document(DocumentId id) noexcept;

 private:
  friend struct DocumentHandleAccess;  // 핸들이 weak_ptr 로 Impl 을 참조하기 위해
  std::shared_ptr<PdfiumWorkerImpl> impl_;
};

namespace detail {

template <class R>
struct SubmitResult {
  using type = Result<R>;
  static type wrap(R&& value) { return type(std::move(value)); }
};

template <class R>
struct SubmitResult<Result<R>> {
  using type = Result<R>;
  static type wrap(Result<R>&& value) { return std::move(value); }
};

}  // namespace detail

template <class F>
auto PdfiumWorker::submit(F&& fn, Priority priority) {
  using Raw = std::remove_cvref_t<std::invoke_result_t<F>>;
  static_assert(!std::is_void_v<Raw>, "submit 의 함수는 값 또는 Result<T> 를 돌려줘야 한다");
  using Wrapper = detail::SubmitResult<Raw>;
  using Out = typename Wrapper::type;

  auto promise = std::make_shared<std::promise<Out>>();
  std::future<Out> future = promise->get_future();
  post(
      [promise, fn = std::forward<F>(fn)](bool cancelled) mutable {
        if (cancelled) {
          promise->set_value(make_error(ErrorCode::kCancelled, "task cancelled"));
          return;
        }
        try {
          promise->set_value(Wrapper::wrap(Raw(fn())));
        } catch (const std::exception& e) {
          promise->set_value(
              make_error(ErrorCode::kUnknown, std::string("task threw: ") + e.what()));
        } catch (...) {
          promise->set_value(make_error(ErrorCode::kUnknown, "task threw unknown exception"));
        }
      },
      priority);
  return future;
}

}  // namespace onpdf::engine
