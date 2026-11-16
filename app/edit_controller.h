#pragma once

#include "onpdf/core/engine/document_handle.h"
#include "onpdf/edit/undo_stack.h"

#include <QObject>
#include <QString>

#include <memory>
#include <string_view>

namespace onpdf::app {

class UiDispatcher;

// 편집 실행기: Command 를 워커(DocumentHandle::modify)로 보내고, 결과를 메인 스레드에서 문서 정보·실행 취소
// 스택에 반영한 뒤 document_changed 로 알린다. 한 번에 하나만 실행한다 (busy 동안 다른 요청은 거절).
class EditController : public QObject {
  Q_OBJECT
 public:
  explicit EditController(UiDispatcher& dispatcher, QObject* parent = nullptr);

  void set_document(engine::DocumentHandle handle);  // 새 문서: 스택 비움, 깨끗한 상태
  void clear();

  // false 면 실행하지 않았다 (문서 없음·실행 중)
  bool run(std::shared_ptr<edit::Command> command);
  bool undo();
  bool redo();

  [[nodiscard]] bool busy() const noexcept { return busy_; }
  [[nodiscard]] bool can_undo() const noexcept { return !busy_ && stack_.can_undo(); }
  [[nodiscard]] bool can_redo() const noexcept { return !busy_ && stack_.can_redo(); }
  [[nodiscard]] std::string_view undo_id() const;  // 없으면 빈 문자열
  [[nodiscard]] std::string_view redo_id() const;
  [[nodiscard]] bool is_modified() const noexcept { return !stack_.is_clean(); }
  void mark_saved();  // 현재 상태를 저장 지점으로

 signals:
  void document_changed(const onpdf::engine::ChangeSet& change);  // 문서 정보는 이미 갱신된 뒤
  void state_changed();                                           // 실행 취소 가능·수정 여부·busy 변화
  void failed(const QString& message);

 private:
  enum class Op { kExecute, kUndo, kRedo };
  bool dispatch(std::shared_ptr<edit::Command> command, Op op);
  void finish(Op op, std::shared_ptr<edit::Command> command, int generation, Result<engine::ChangeSet> result);

  UiDispatcher& dispatcher_;
  engine::DocumentHandle handle_;
  edit::UndoStack stack_;
  bool busy_ = false;
  int generation_ = 0;  // 문서 교체 시 증가: 이전 문서의 늦은 결과를 버린다
};

}  // namespace onpdf::app
