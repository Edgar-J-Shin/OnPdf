#pragma once

// 구조 작업(qpdf 병합·분할)을 PDFium 워커가 아닌 전용 스레드에서 돌리고 결과를 메인 스레드로 넘긴다.
// qpdf 작업은 중단 API 가 없어 취소는 "결과 버리기" 다 — 호출자가 finished(cancelled=true) 에서 출력 파일을 지운다.
// 한 번에 하나만 돈다

#include "onpdf/core/engine/qpdf_tools.h"
#include "onpdf/core/engine/save.h"

#include <QObject>

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

namespace onpdf::app {

class UiDispatcher;

class StructureJob : public QObject {
  Q_OBJECT
 public:
  using Outcome = onpdf::Result<std::vector<engine::SaveResult>>;
  // 작업 함수는 작업 스레드에서 실행된다. progress 콜백(0..100)은 메인으로 전달된다
  using Work = std::function<Outcome(const engine::ProgressFn& progress)>;

  explicit StructureJob(UiDispatcher& dispatcher, QObject* parent = nullptr);
  ~StructureJob() override;  // 진행 중이면 끝날 때까지 기다린다 (qpdf 작업은 초 단위)

  bool start(Work work);  // 이미 돌고 있으면 false
  [[nodiscard]] bool active() const noexcept { return active_.load(); }
  void cancel() noexcept { cancelled_.store(true); }
  [[nodiscard]] bool cancelled() const noexcept { return cancelled_.load(); }
  // 마지막 작업의 결과 (finished 뒤에 유효). Result 는 Qt 신호 인자로 쓸 수 없어(메타타입 비교 연산자 요구) 여기서 읽는다
  [[nodiscard]] const Outcome& outcome() const noexcept { return outcome_; }

 signals:
  void progress(int percent);
  void finished(bool cancelled);

 private:
  UiDispatcher& dispatcher_;
  std::jthread thread_;
  std::atomic<bool> active_{false};
  std::atomic<bool> cancelled_{false};
  int generation_ = 0;
  Outcome outcome_ = std::unexpected(onpdf::Error{onpdf::ErrorCode::kInvalidState, "not run"});
};

}  // namespace onpdf::app
