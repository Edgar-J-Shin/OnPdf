#pragma once

#include "onpdf/core/engine/document_handle.h"
#include "onpdf/edit/undo_stack.h"

#include <QObject>
#include <QString>
#include <memory>
#include <string_view>
#include <vector>

namespace onpdf::app {

class UiDispatcher;

// 편집 실행기: Command 를 워커(DocumentHandle::modify)로 보내고, 결과를 메인 스레드에서 문서
// 정보·실행 취소 스택에 반영한 뒤 document_changed 로 알린다. 한 번에 하나만 실행한다 (busy 동안
// 다른 요청은 거절).
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
  // 다음 실행 취소 대상 (없으면 nullptr). 앱이 안내 문구를 고를 때만 읽는다
  [[nodiscard]] std::shared_ptr<const edit::Command> undo_command() const {
    return stack_.undo_target();
  }
  [[nodiscard]] bool is_modified() const noexcept { return !stack_.is_clean(); }
  void mark_saved();  // 현재 상태를 저장 지점으로

 signals:
  void document_changed(const onpdf::engine::ChangeSet& change);  // 문서 정보는 이미 갱신된 뒤
  void state_changed();  // 실행 취소 가능·수정 여부·busy 변화
  void failed(onpdf::ErrorCode code, const QString& message);

 private:
  enum class Op { kExecute, kUndo, kRedo };
  bool dispatch(std::shared_ptr<edit::Command> command, Op op);
  void finish(Op op, std::shared_ptr<edit::Command> command, int generation,
              Result<engine::ChangeSet> result);
  // 스택에서 밀려난 Command 의 discard 를 워커에서 부른다 (문서가 보관한 스냅샷 해제). busy 와
  // 무관, 결과 무시
  void discard_on_worker(std::vector<std::shared_ptr<edit::Command>> evicted);
  // 실패한 명령 뒤: 워커에서 실제 쪽 수·크기를 읽어 문서 정보와 다르면 맞추고(document_changed)
  // 스택을 무효화한다. 쪽 단위로 진행되는 실행 취소가 중간에 실패하면 문서가 반쯤 바뀐 채 남을 수
  // 있다 (PDFium 은 롤백이 없다)
  void resync_after_failure();

  UiDispatcher& dispatcher_;
  engine::DocumentHandle handle_;
  edit::UndoStack stack_;
  bool busy_ = false;
  int generation_ = 0;  // 문서 교체 시 증가: 이전 문서의 늦은 결과를 버린다
};

}  // namespace onpdf::app
