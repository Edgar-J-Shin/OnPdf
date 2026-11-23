#pragma once

// 문서 변경의 유일한 경로. execute/undo 는 워커 스레드에서 EditableDocument 에 적용된다.
// Command 객체는 실행 취소 스택(메인 스레드)과 워커 작업이 shared_ptr 로 함께 잡는다. 내부
// 상태(되돌릴 값)는 워커에서만 쓰고, 같은 Command 의 execute/undo 는 실행 취소 스택의 busy 규칙으로
// 겹치지 않는다.

#include "onpdf/core/engine/editable_document.h"
#include "onpdf/core/error.h"

#include <string_view>

namespace onpdf::edit {

class Command {
 public:
  Command() = default;
  Command(const Command&) = delete;
  Command& operator=(const Command&) = delete;
  virtual ~Command() = default;

  // 종류 식별자 (예: "rotate_page"). 사용자에게 보일 이름은 앱이 tr() 로 바꾼다 (i18n)
  [[nodiscard]] virtual std::string_view id() const noexcept = 0;
  virtual Result<engine::ChangeSet> execute(engine::EditableDocument& doc) = 0;
  virtual Result<engine::ChangeSet> undo(engine::EditableDocument& doc) = 0;
  // 실행 취소 스택에서 밀려나 더 쓰이지 않을 때 워커에서 호출된다: 문서에 보관한 스냅샷을 해제한다.
  // 기본은 빈 구현
  virtual void discard(engine::EditableDocument& doc) noexcept { (void)doc; }
  // 방금 실행된 next 를 이 Command 에 합칠 수 있으면 합치고 true (예: 화살표 키 연속 이동, #49).
  // true 면 스택은 next 를 따로 올리지 않는다 — 이 Command 하나의 undo 가 둘 다 되돌려야 한다.
  // 기본은 false
  virtual bool merge_with(const Command& next) {
    (void)next;
    return false;
  }
};

}  // namespace onpdf::edit
