#include "edit_controller.h"

#include "onpdf/edit/page_ops.h"
#include "ui_dispatcher.h"

#include <QPointer>

#include <utility>

namespace onpdf::app {

EditController::EditController(UiDispatcher& dispatcher, QObject* parent) : QObject(parent), dispatcher_(dispatcher) {}

void EditController::set_document(engine::DocumentHandle handle) {
  handle_ = std::move(handle);
  stack_.clear();
  busy_ = false;
  ++generation_;
  emit state_changed();
}

void EditController::clear() { set_document(engine::DocumentHandle{}); }

bool EditController::run(std::shared_ptr<edit::Command> command) { return dispatch(std::move(command), Op::kExecute); }

bool EditController::undo() { return can_undo() && dispatch(stack_.undo_target(), Op::kUndo); }

bool EditController::redo() { return can_redo() && dispatch(stack_.redo_target(), Op::kRedo); }

std::string_view EditController::undo_id() const {
  const auto target = stack_.undo_target();
  return target ? target->id() : std::string_view{};
}

std::string_view EditController::redo_id() const {
  const auto target = stack_.redo_target();
  return target ? target->id() : std::string_view{};
}

void EditController::mark_saved() {
  stack_.set_clean();
  emit state_changed();
}

bool EditController::dispatch(std::shared_ptr<edit::Command> command, Op op) {
  if (!command || busy_ || !handle_.valid()) return false;
  busy_ = true;
  emit state_changed();
  const int generation = generation_;
  UiDispatcher* dispatcher = &dispatcher_;
  QPointer<EditController> self(this);
  // Command 객체는 이 람다와 스택이 함께 잡는다. execute/undo 는 워커에서만, busy 동안 하나씩 실행된다
  handle_.modify(
      [command, op](engine::EditableDocument& doc) {
        return op == Op::kUndo ? command->undo(doc) : command->execute(doc);
      },
      // 워커 스레드: 메인으로 넘기기만
      [dispatcher, self, op, command, generation](Result<engine::ChangeSet> result) {
        auto shared = std::make_shared<Result<engine::ChangeSet>>(std::move(result));
        dispatcher->post_to(self, [op, command, generation, shared](EditController* controller) {
          controller->finish(op, command, generation, std::move(*shared));
        });
      });
  return true;
}

void EditController::finish(Op op, std::shared_ptr<edit::Command> command, int generation,
                            Result<engine::ChangeSet> result) {
  if (generation != generation_) return;  // 그 사이 문서가 바뀌었다
  busy_ = false;
  if (!result) {
    if (op == Op::kExecute) discard_on_worker({std::move(command)});  // 스택에 오르지 않으므로 스냅샷을 해제한다
    resync_after_failure();
    emit state_changed();
    emit failed(result.error().code, QString::fromStdString(result.error().message));
    return;
  }
  handle_.apply_change(*result);  // 메인 스레드에서만 문서 정보를 고친다
  switch (op) {
    case Op::kExecute: discard_on_worker(stack_.push(std::move(command))); break;
    case Op::kUndo: stack_.did_undo(); break;
    case Op::kRedo: stack_.did_redo(); break;
  }
  emit document_changed(*result);
  emit state_changed();
}

void EditController::resync_after_failure() {
  if (!handle_.valid()) return;
  const int generation = generation_;
  UiDispatcher* dispatcher = &dispatcher_;
  QPointer<EditController> self(this);
  handle_.modify([](engine::EditableDocument& doc) { return edit::structural_change(doc); },
                 // 워커 스레드: 메인으로 넘기기만
                 [dispatcher, self, generation](Result<engine::ChangeSet> result) {
                   auto shared = std::make_shared<Result<engine::ChangeSet>>(std::move(result));
                   dispatcher->post_to(self, [generation, shared](EditController* controller) {
                     if (generation != controller->generation_ || !*shared) return;
                     const auto& actual = (*shared)->all_sizes;
                     const auto& known = controller->handle_.info().page_sizes;
                     bool same = actual.size() == known.size();
                     for (std::size_t i = 0; same && i < actual.size(); ++i) {
                       same = actual[i].width == known[i].width && actual[i].height == known[i].height;
                     }
                     if (same) return;  // 문서는 그대로다 (예: 인자 검증 단계 실패)
                     // 문서가 실제로 바뀌었다: 화면을 맞추고, 스택의 기록은 더 믿을 수 없으니 버린다 (저장할 때까지 수정됨)
                     controller->handle_.apply_change(**shared);
                     controller->stack_.invalidate();
                     emit controller->document_changed(**shared);
                     emit controller->state_changed();
                   });
                 });
}

void EditController::discard_on_worker(std::vector<std::shared_ptr<edit::Command>> evicted) {
  if (evicted.empty() || !handle_.valid()) return;
  handle_.modify(
      [evicted = std::move(evicted)](engine::EditableDocument& doc) -> Result<engine::ChangeSet> {
        for (const auto& command : evicted) command->discard(doc);
        return engine::ChangeSet{};
      },
      [](Result<engine::ChangeSet>) {});
}

}  // namespace onpdf::app
