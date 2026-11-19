#pragma once

// 텍스트 상자 편집 Command (#47): 이동·삭제·수정. 상자 = 마크 box 로 묶인 줄 객체들.
// 수정·삭제 취소는 FPDFText_SetText 를 쓰지 않고 삭제 + 재생성(AddText 와 같은 배치·서브셋 파이프라인)으로 한다 —
// 글리프 집합이 바뀌면 서브셋 폰트가 달라지기 때문. 줄 id 는 순서대로 재사용해 재실행이 결정적이다

#include "onpdf/core/engine/text_object.h"
#include "onpdf/edit/add_text_command.h"
#include "onpdf/edit/command.h"
#include "onpdf/edit/font_face.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace onpdf::edit {

// 문서에서 읽어 온 상자 내용 (재생성에 필요한 전부)
struct TextBoxContent {
  std::vector<std::string> ids;  // 줄 순서
  std::string box;
  TextBoxSpec spec;              // page·origin(상자 왼쪽 위)·size·color·utf8 (face 는 호출자가 채운다)
};

// 상자의 줄들을 모아 TextBoxContent 로. origin = 첫 줄 기준선 − ascent(size). 상자가 없으면 kInvalidArgument
[[nodiscard]] Result<TextBoxContent> collect_text_box(engine::EditableDocument& doc, int page, const std::string& box,
                                                      const std::shared_ptr<const FontFace>& face);

// TextBoxSpec → 엔진 스펙 (배치·서브셋·ToUnicode). ids/box 를 주면 그대로 쓴다 (재생성·재실행)
[[nodiscard]] Result<engine::TextObjectsSpec> build_text_objects_spec(const TextBoxSpec& spec, const std::string& box,
                                                                      const std::vector<std::string>& ids);

// 이동: 쪽 포인트 Δ. 취소 = −Δ
class MoveTextBoxCommand final : public Command {
 public:
  MoveTextBoxCommand(int page, std::vector<std::string> ids, Point delta);
  [[nodiscard]] std::string_view id() const noexcept override { return "move_text"; }
  Result<engine::ChangeSet> execute(engine::EditableDocument& doc) override;
  Result<engine::ChangeSet> undo(engine::EditableDocument& doc) override;

 private:
  int page_;
  std::vector<std::string> ids_;
  Point delta_;
  bool executed_ = false;
};

// 삭제: 실행 전 내용을 모아 두고, 취소 때 같은 줄 id·상자 id 로 다시 만든다 (같은 글자 집합 → 폰트 캐시 재사용)
class RemoveTextBoxCommand final : public Command {
 public:
  RemoveTextBoxCommand(int page, std::string box, std::shared_ptr<const FontFace> face);
  [[nodiscard]] std::string_view id() const noexcept override { return "remove_text"; }
  Result<engine::ChangeSet> execute(engine::EditableDocument& doc) override;
  Result<engine::ChangeSet> undo(engine::EditableDocument& doc) override;
  [[nodiscard]] const TextBoxContent& content() const noexcept { return content_; }

 private:
  int page_;
  std::string box_;
  std::shared_ptr<const FontFace> face_;
  TextBoxContent content_;
  bool executed_ = false;
};

// 수정: 내용·크기·색 중 준 것만 바꾼다 (삭제 + 재생성). 줄 id 는 순서대로 재사용, 모자라면 새 id, 남으면 버림
class ReplaceTextBoxCommand final : public Command {
 public:
  ReplaceTextBoxCommand(int page, std::string box, std::shared_ptr<const FontFace> face, std::optional<std::string> utf8,
                        std::optional<double> size, std::optional<engine::Color> color);
  [[nodiscard]] std::string_view id() const noexcept override { return "replace_text"; }
  Result<engine::ChangeSet> execute(engine::EditableDocument& doc) override;
  Result<engine::ChangeSet> undo(engine::EditableDocument& doc) override;
  [[nodiscard]] const std::vector<std::string>& after_ids() const noexcept { return after_ids_; }

 private:
  int page_;
  std::string box_;
  std::shared_ptr<const FontFace> face_;
  std::optional<std::string> utf8_;
  std::optional<double> size_;
  std::optional<engine::Color> color_;
  TextBoxContent before_;
  std::vector<std::string> after_ids_;
  bool executed_ = false;
};

}  // namespace onpdf::edit
