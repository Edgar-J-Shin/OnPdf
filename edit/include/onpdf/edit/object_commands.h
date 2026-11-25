#pragma once

// 쪽 객체(텍스트 상자 줄·이미지) 공용 Command (#49): 이미지 추가, 이동(키보드 연속 이동 병합),
// 이미지 크기, 삭제. 삭제·추가 취소는 객체를 다시 만들지 않고 문서가 떼어 보관한 객체를 다시 붙인다
// (Command 는 DetachedId 만 쥔다). 다른 프로그램이 넣은 이미지("#obj<index>")는 첫 실행에서
// mark_object 로 마크를 붙이고 그 id 로 바꿔 쥔다

#include "onpdf/core/engine/page_object.h"
#include "onpdf/edit/command.h"

#include <string>
#include <vector>

namespace onpdf::edit {

// 이미지 추가: 첫 실행은 픽셀/JPEG 를 넣고(그 뒤 데이터를 버린다), 취소는 떼어 보관, 재실행은 다시
// 붙인다
class AddImageCommand final : public Command {
 public:
  AddImageCommand(int page, engine::ImageSpec spec);
  [[nodiscard]] std::string_view id() const noexcept override { return "add_image"; }
  Result<engine::ChangeSet> execute(engine::EditableDocument& doc) override;
  Result<engine::ChangeSet> undo(engine::EditableDocument& doc) override;
  void discard(engine::EditableDocument& doc) noexcept override;
  [[nodiscard]] const std::string& object_id() const noexcept { return object_id_; }

 private:
  int page_;
  engine::ImageSpec spec_;
  std::string object_id_;
  engine::DetachedId detached_ = 0;  // 취소된 상태에서만 보관
  bool executed_ = false;
};

// 이동 (텍스트 상자 줄·이미지): 취소 = −Δ. keyboard 이동끼리는 같은 쪽·같은 객체면 하나로 합친다
class MoveObjectsCommand final : public Command {
 public:
  MoveObjectsCommand(int page, std::vector<std::string> ids, Point delta, bool keyboard = false);
  [[nodiscard]] std::string_view id() const noexcept override { return "move_object"; }
  Result<engine::ChangeSet> execute(engine::EditableDocument& doc) override;
  Result<engine::ChangeSet> undo(engine::EditableDocument& doc) override;
  bool merge_with(const Command& next) override;
  [[nodiscard]] const std::vector<std::string>& ids() const noexcept { return ids_; }
  [[nodiscard]] Point delta() const noexcept { return delta_; }

 private:
  int page_;
  std::vector<std::string> ids_;
  Point delta_;
  bool keyboard_;
  bool executed_ = false;
};

// 이미지 크기·자리: 이전·이후 사각형 (쪽 포인트)
class ResizeObjectCommand final : public Command {
 public:
  ResizeObjectCommand(int page, std::string id, Rect before, Rect after);
  [[nodiscard]] std::string_view id() const noexcept override { return "resize_object"; }
  Result<engine::ChangeSet> execute(engine::EditableDocument& doc) override;
  Result<engine::ChangeSet> undo(engine::EditableDocument& doc) override;
  [[nodiscard]] const std::string& object_id() const noexcept { return object_id_; }

 private:
  int page_;
  std::string object_id_;
  Rect before_;
  Rect after_;
  bool executed_ = false;
};

// 삭제 (텍스트 상자 줄들·이미지): 떼어 문서가 보관, 취소는 같은 자리에 다시 붙인다
class RemoveObjectsCommand final : public Command {
 public:
  RemoveObjectsCommand(int page, std::vector<std::string> ids);
  [[nodiscard]] std::string_view id() const noexcept override { return "remove_object"; }
  Result<engine::ChangeSet> execute(engine::EditableDocument& doc) override;
  Result<engine::ChangeSet> undo(engine::EditableDocument& doc) override;
  void discard(engine::EditableDocument& doc) noexcept override;
  [[nodiscard]] const std::vector<std::string>& ids() const noexcept { return ids_; }

 private:
  int page_;
  std::vector<std::string> ids_;
  engine::DetachedId detached_ = 0;  // 실행된 상태에서만 보관
  bool executed_ = false;
};

}  // namespace onpdf::edit
