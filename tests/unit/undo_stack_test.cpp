#include "onpdf/edit/undo_stack.h"

#include <gtest/gtest.h>

#include <memory>
#include <string_view>

namespace onpdf::edit {
namespace {

class NoopCommand final : public Command {
 public:
  explicit NoopCommand(int tag) : tag(tag) {}
  [[nodiscard]] std::string_view id() const noexcept override { return "noop"; }
  Result<engine::ChangeSet> execute(engine::EditableDocument&) override { return engine::ChangeSet{}; }
  Result<engine::ChangeSet> undo(engine::EditableDocument&) override { return engine::ChangeSet{}; }
  int tag;
};

std::shared_ptr<Command> cmd(int tag) { return std::make_shared<NoopCommand>(tag); }
int tag_of(const std::shared_ptr<Command>& c) { return c ? static_cast<NoopCommand&>(*c).tag : -1; }

TEST(UndoStack, UndoRedoOrder) {
  UndoStack stack;
  EXPECT_FALSE(stack.can_undo());
  EXPECT_TRUE(stack.is_clean());
  stack.push(cmd(1));
  stack.push(cmd(2));
  EXPECT_EQ(tag_of(stack.undo_target()), 2);
  EXPECT_FALSE(stack.can_redo());
  stack.did_undo();
  EXPECT_EQ(tag_of(stack.undo_target()), 1);
  EXPECT_EQ(tag_of(stack.redo_target()), 2);
  stack.did_redo();
  EXPECT_EQ(tag_of(stack.undo_target()), 2);
  EXPECT_FALSE(stack.is_clean());
}

TEST(UndoStack, NewCommandDropsRedoHistory) {
  UndoStack stack;
  stack.push(cmd(1));
  stack.push(cmd(2));
  stack.did_undo();
  stack.push(cmd(3));
  EXPECT_FALSE(stack.can_redo());
  EXPECT_EQ(stack.size(), 2u);
  EXPECT_EQ(tag_of(stack.undo_target()), 3);
}

TEST(UndoStack, CleanTracksSavePoint) {
  UndoStack stack;
  stack.push(cmd(1));
  EXPECT_FALSE(stack.is_clean());
  stack.set_clean();  // 저장
  EXPECT_TRUE(stack.is_clean());
  stack.did_undo();   // 저장 뒤 실행 취소 → 수정됨
  EXPECT_FALSE(stack.is_clean());
  stack.did_redo();   // 다시 저장 지점
  EXPECT_TRUE(stack.is_clean());
}

TEST(UndoStack, BranchingAwayFromSavePointNeverCleanAgain) {
  UndoStack stack;
  stack.push(cmd(1));
  stack.push(cmd(2));
  stack.set_clean();  // 2 까지 적용한 상태로 저장
  stack.did_undo();
  stack.push(cmd(3));  // 저장 지점(2)이 다시 실행 기록에서 사라짐
  EXPECT_FALSE(stack.is_clean());
  stack.did_undo();
  EXPECT_FALSE(stack.is_clean());
}

TEST(UndoStack, LimitDropsOldestAndSavePoint) {
  UndoStack stack(3);
  stack.push(cmd(1));
  stack.push(cmd(2));
  stack.push(cmd(3));
  stack.push(cmd(4));
  EXPECT_EQ(stack.size(), 3u);
  stack.did_undo();
  stack.did_undo();
  stack.did_undo();
  EXPECT_FALSE(stack.can_undo());
  EXPECT_EQ(tag_of(stack.redo_target()), 2);  // 1 은 버려졌다
  EXPECT_FALSE(stack.is_clean()) << "save point (empty document) was trimmed away";
}

TEST(UndoStack, ClearResets) {
  UndoStack stack;
  stack.push(cmd(1));
  stack.clear();
  EXPECT_FALSE(stack.can_undo());
  EXPECT_FALSE(stack.can_redo());
  EXPECT_TRUE(stack.is_clean());
}


// push 는 버려진 다시 실행 기록과 상한 초과분을 돌려준다 (호출자가 워커에서 discard 를 부른다)
TEST(UndoStack, PushReturnsEvictedCommands) {
  UndoStack stack(2);
  auto a = cmd(1);
  auto b = cmd(2);
  EXPECT_TRUE(stack.push(a).empty());
  EXPECT_TRUE(stack.push(b).empty());
  auto over = stack.push(cmd(3));  // 상한 2 → a 가 밀려난다
  ASSERT_EQ(over.size(), 1u);
  EXPECT_EQ(over[0], a);
  stack.did_undo();
  stack.did_undo();  // b, 3 모두 취소 → 둘 다 다시 실행 기록
  auto branched = stack.push(cmd(4));
  ASSERT_EQ(branched.size(), 2u);
  EXPECT_EQ(branched[0], b);
  EXPECT_TRUE(stack.push(nullptr).empty());
}

}  // namespace
}  // namespace onpdf::edit
