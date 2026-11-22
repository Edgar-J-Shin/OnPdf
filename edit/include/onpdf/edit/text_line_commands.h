#pragma once

// 기존 텍스트 줄 수정·삭제 Command (#65, M4). 엔진이 qpdf 콘텐츠 수술로 줄을 지우고 문서를 다시 열며, 수술 전 바이트를
// 상태 토큰으로 보관한다. Command 는 토큰만 쥔다(취소 = 토큰 복원, 다시 실행 = 재수술). OnPdf 상자(원본 글꼴로 고친 줄)를
// 다시 고칠 때는 수술이 없으므로 취소는 이전 글로 다시 교체한다.

#include "onpdf/edit/command.h"
#include "onpdf/edit/font_face.h"
#include "onpdf/core/engine/text_line.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace onpdf::edit {

class ReplaceTextLineCommand final : public Command {
 public:
  // utf8·size·color 가운데 바꿀 것만 (모두 비면 실패). size 는 쪽 포인트 유효 크기, color 는 RGB.
  // fallback_face: 원본 글꼴이 글자를 못 그릴 때 쓸 번들 글꼴 (nullptr 이면 원본 글꼴만)
  ReplaceTextLineCommand(int page, std::string line_id, std::optional<std::string> utf8, std::optional<double> size,
                         std::optional<engine::Color> color, std::shared_ptr<const FontFace> fallback_face);
  // 글이 바뀌면 "replace_line", 서식만이면 "format_line" (#69)
  [[nodiscard]] std::string_view id() const noexcept override { return utf8_ ? "replace_line" : "format_line"; }
  Result<engine::ChangeSet> execute(engine::EditableDocument& doc) override;
  Result<engine::ChangeSet> undo(engine::EditableDocument& doc) override;
  void discard(engine::EditableDocument& doc) noexcept override;
  [[nodiscard]] bool used_fallback() const noexcept { return used_fallback_; }
  [[nodiscard]] std::string fallback_family() const { return face_ ? face_->family_name() : std::string(); }  // 안내용 (#73)
  [[nodiscard]] const std::string& box() const noexcept { return box_; }
  [[nodiscard]] const std::vector<std::string>& ids() const noexcept { return ids_; }
  [[nodiscard]] int page() const noexcept { return page_; }

 private:
  int page_;
  std::string line_id_;
  std::optional<std::string> utf8_;
  std::optional<double> size_;
  std::optional<engine::Color> color_;
  std::shared_ptr<const FontFace> face_;
  engine::DocumentStateId before_ = 0;
  bool executed_ = false;
  bool used_fallback_ = false;
  std::string box_;
  std::vector<std::string> ids_;
  std::string previous_utf8_;  // OnPdf 상자 경로: 취소는 이전 글·크기·색으로 다시 교체
  double previous_size_ = 0.0;
  engine::Color previous_color_;
  bool onpdf_box_ = false;
};

class RemoveTextLineCommand final : public Command {
 public:
  RemoveTextLineCommand(int page, std::string line_id);
  [[nodiscard]] std::string_view id() const noexcept override { return "remove_line"; }
  Result<engine::ChangeSet> execute(engine::EditableDocument& doc) override;
  Result<engine::ChangeSet> undo(engine::EditableDocument& doc) override;
  void discard(engine::EditableDocument& doc) noexcept override;

 private:
  int page_;
  std::string line_id_;
  engine::DocumentStateId before_ = 0;
  bool executed_ = false;
};

}  // namespace onpdf::edit
