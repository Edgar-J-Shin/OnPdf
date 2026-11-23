#include "structure_job.h"

#include "ui_dispatcher.h"
#include <QPointer>
#include <memory>
#include <utility>

namespace onpdf::app {

StructureJob::StructureJob(UiDispatcher& dispatcher, QObject* parent)
    : QObject(parent), dispatcher_(dispatcher) {}

StructureJob::~StructureJob() {
  ++generation_;  // 늦게 오는 결과는 버린다 (jthread 소멸이 join 한다)
}

bool StructureJob::start(Work work) {
  if (active_.load() || !work) return false;
  if (thread_.joinable()) thread_.join();  // 이전 작업 스레드 정리
  active_.store(true);
  cancelled_.store(false);
  const int generation = ++generation_;
  UiDispatcher* dispatcher = &dispatcher_;
  QPointer<StructureJob> self(this);
  thread_ = std::jthread([work = std::move(work), dispatcher, self, generation] {
    const engine::ProgressFn progress = [dispatcher, self, generation](int percent) {
      dispatcher->post_to(self, [percent, generation](StructureJob* job) {
        if (generation == job->generation_) emit job->progress(percent);
      });
    };
    auto outcome = std::make_shared<Outcome>(work(progress));
    // 작업 스레드: 메인으로 넘기기만
    dispatcher->post_to(self, [outcome, generation](StructureJob* job) {
      if (generation != job->generation_) return;
      job->outcome_ = std::move(*outcome);
      job->active_.store(false);
      emit job->finished(job->cancelled_.load());
    });
  });
  return true;
}

}  // namespace onpdf::app
