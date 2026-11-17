#pragma once

// 주석 Command 4종. 모두 비구조 변경(ChangeSet.pages 만). 문서 안 정체성은 /NM 이라 Command 는 id 와 값만 쥔다

#include "onpdf/core/engine/annotation.h"
#include "onpdf/edit/command.h"

#include <string>
#include <utility>
#include <vector>

namespace onpdf::edit {

// 종류별 명령 id ("add_highlight" …). 앱이 tr() 라벨로 바꾼다
[[nodiscard]] std::string_view add_command_id(engine::AnnotationKind kind) noexcept;

// 주석 추가 (여러 쪽에 걸친 마크업도 한 단위). 실패하면 그때까지 만든 것을 지운다(보상)
class AddAnnotationsCommand final : public Command {
 public:
  explicit AddAnnotationsCommand(std::vector<std::pair<int, engine::AnnotationSpec>> items);
  [[nodiscard]] std::string_view id() const noexcept override { return id_; }
  Result<engine::ChangeSet> execute(engine::EditableDocument& doc) override;
  Result<engine::ChangeSet> undo(engine::EditableDocument& doc) override;
  [[nodiscard]] const std::vector<std::pair<int, engine::AnnotationSpec>>& items() const noexcept { return items_; }

 private:
  std::vector<std::pair<int, engine::AnnotationSpec>> items_;  // spec.id 는 첫 실행 뒤 NM 으로 채워져 재실행에 쓰인다
  std::string_view id_;
  bool executed_ = false;
};

// 주석 삭제: 실행 전 원시값 스냅샷 → 취소 때 같은 NM 으로 재생성
class RemoveAnnotationCommand final : public Command {
 public:
  RemoveAnnotationCommand(int page, std::string id);
  [[nodiscard]] std::string_view id() const noexcept override { return "remove_annotation"; }
  Result<engine::ChangeSet> execute(engine::EditableDocument& doc) override;
  Result<engine::ChangeSet> undo(engine::EditableDocument& doc) override;
  [[nodiscard]] const std::string& annotation_id() const noexcept { return annotation_id_; }

 private:
  int page_;
  std::string annotation_id_;  // 실행 뒤에는 /NM
  engine::AnnotationSnapshot snapshot_;
  bool executed_ = false;
};

// 색 변경: 이전 AP(있으면)와 색을 보관해 취소 때 모양 그대로 되돌린다
class SetAnnotationColorCommand final : public Command {
 public:
  SetAnnotationColorCommand(int page, std::string id, engine::Color color);
  [[nodiscard]] std::string_view id() const noexcept override { return "set_annotation_color"; }
  Result<engine::ChangeSet> execute(engine::EditableDocument& doc) override;
  Result<engine::ChangeSet> undo(engine::EditableDocument& doc) override;

 private:
  int page_;
  std::string annotation_id_;
  engine::Color color_;
  engine::AnnotationSnapshot before_;
  bool executed_ = false;
};

class SetAnnotationContentsCommand final : public Command {
 public:
  SetAnnotationContentsCommand(int page, std::string id, std::string contents_utf8);
  [[nodiscard]] std::string_view id() const noexcept override { return "set_annotation_contents"; }
  Result<engine::ChangeSet> execute(engine::EditableDocument& doc) override;
  Result<engine::ChangeSet> undo(engine::EditableDocument& doc) override;

 private:
  int page_;
  std::string annotation_id_;
  std::string contents_;
  std::string before_;
  bool executed_ = false;
};

}  // namespace onpdf::edit
