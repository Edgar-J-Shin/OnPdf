#include "onpdf/edit/undo_stack.h"

#include <algorithm>
#include <utility>

namespace onpdf::edit {

UndoStack::UndoStack(std::size_t limit) : limit_(std::max<std::size_t>(limit, 1)) {}

std::vector<std::shared_ptr<Command>> UndoStack::push(std::shared_ptr<Command> executed) {
  std::vector<std::shared_ptr<Command>> evicted;
  if (!executed) return evicted;
  // 병합: 다시 실행 기록이 없고, 맨 위가 저장 지점이 아니며, 맨 위가 합치기를 받아들이면 따로
  // 올리지 않는다
  if (index_ > 0 && index_ == items_.size() && (!clean_index_ || *clean_index_ != index_) &&
      items_.back()->merge_with(*executed)) {
    evicted.push_back(std::move(
        executed));  // 합쳐진 명령은 스택에 오르지 않는다 → 호출자가 discard (상태 토큰 해제, #79)
    return evicted;
  }
  // 다시 실행 기록 버리기. 저장 지점이 그 안에 있었으면 다시는 도달할 수 없다
  if (index_ < items_.size()) {
    if (clean_index_ && *clean_index_ > index_) clean_index_.reset();
    const auto first = items_.begin() + static_cast<std::ptrdiff_t>(index_);
    evicted.assign(first, items_.end());
    items_.erase(first, items_.end());
  }
  items_.push_back(std::move(executed));
  ++index_;
  if (items_.size() > limit_) {
    evicted.push_back(std::move(items_.front()));
    items_.erase(items_.begin());
    --index_;
    if (clean_index_) {
      if (*clean_index_ == 0) {
        clean_index_.reset();  // 저장 지점이 잘려 나갔다
      } else {
        --*clean_index_;
      }
    }
  }
  return evicted;
}

std::shared_ptr<Command> UndoStack::undo_target() const {
  return can_undo() ? items_[index_ - 1] : nullptr;
}

std::shared_ptr<Command> UndoStack::redo_target() const {
  return can_redo() ? items_[index_] : nullptr;
}

void UndoStack::did_undo() {
  if (can_undo()) --index_;
}

void UndoStack::did_redo() {
  if (can_redo()) ++index_;
}

void UndoStack::clear() {
  items_.clear();
  index_ = 0;
  clean_index_ = 0;
}

void UndoStack::invalidate() {
  items_.clear();
  index_ = 0;
  clean_index_.reset();
}

}  // namespace onpdf::edit
