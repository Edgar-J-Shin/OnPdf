#pragma once

// 실행 취소·다시 실행 스택. 메인 스레드 전용 (Command 실행은 호출자가 워커에서 하고 결과만 알린다).
// 저장 지점(clean)을 추적해 "수정됨" 을 판정한다.

#include "onpdf/edit/command.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace onpdf::edit {

class UndoStack {
 public:
  static constexpr std::size_t kDefaultLimit = 100;
  explicit UndoStack(std::size_t limit = kDefaultLimit);

  // 실행에 성공한 Command 를 올린다. 다시 실행 기록은 지운다. 상한을 넘으면 가장 오래된 것을
  // 버린다. 버린 Command(다시 실행 기록·상한 초과분)를 돌려준다 — 호출자가 워커에서 discard 를 불러
  // 스냅샷을 해제한다
  std::vector<std::shared_ptr<Command>> push(std::shared_ptr<Command> executed);
  [[nodiscard]] std::shared_ptr<Command> undo_target() const;  // 없으면 nullptr
  [[nodiscard]] std::shared_ptr<Command> redo_target() const;
  void did_undo();  // undo_target 의 undo 가 성공했다
  void did_redo();  // redo_target 의 execute 가 성공했다

  [[nodiscard]] bool can_undo() const noexcept { return index_ > 0; }
  [[nodiscard]] bool can_redo() const noexcept { return index_ < items_.size(); }
  [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }

  void set_clean() noexcept { clean_index_ = index_; }
  // 저장 지점과 같은 위치면 깨끗하다. 저장 지점이 버려졌으면(분기·상한) 다시는 깨끗해지지 않는다
  [[nodiscard]] bool is_clean() const noexcept { return clean_index_ && *clean_index_ == index_; }
  void clear();       // 새 문서: 비우고 깨끗한 상태
  void invalidate();  // 기록을 믿을 수 없다(실패한 명령이 문서를 반쯤 바꿈): 비우고, 저장할 때까지
                      // 수정됨으로 둔다

 private:
  std::vector<std::shared_ptr<Command>> items_;
  std::size_t index_ = 0;  // 다음 다시 실행 위치 (= 적용된 Command 수)
  std::optional<std::size_t> clean_index_ = 0;
  std::size_t limit_;
};

}  // namespace onpdf::edit
