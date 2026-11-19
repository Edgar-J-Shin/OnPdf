#pragma once

// 텍스트 상자 추가 (#45). execute(워커)에서 배치 → 서브셋 → ToUnicode 를 만들어 엔진에 넘긴다.
// 실행 취소는 줄별 객체 id 로 지운다. 재실행은 같은 id·같은 서브셋 key(문서가 폰트를 재사용).
// Command 는 PDFium·HarfBuzz 가변 객체를 쥐지 않는다 (FontFace 는 불변 공유)

#include "onpdf/core/engine/annotation.h"  // Color
#include "onpdf/core/geometry.h"
#include "onpdf/edit/command.h"
#include "onpdf/edit/font_face.h"

#include <memory>
#include <string>
#include <vector>

namespace onpdf::edit {

struct TextBoxSpec {
  int page = 0;
  Point origin;                 // 상자 왼쪽 위 (쪽 포인트)
  double size = 24.0;           // pt
  engine::Color color{0, 0, 0, 255};
  std::string utf8;             // 여러 줄은 '\n'
  std::shared_ptr<const FontFace> face;
};

class AddTextCommand final : public Command {
 public:
  explicit AddTextCommand(TextBoxSpec spec);
  [[nodiscard]] std::string_view id() const noexcept override { return "add_text"; }
  Result<engine::ChangeSet> execute(engine::EditableDocument& doc) override;
  Result<engine::ChangeSet> undo(engine::EditableDocument& doc) override;
  [[nodiscard]] const std::vector<std::string>& object_ids() const noexcept { return ids_; }
  [[nodiscard]] const TextBoxSpec& spec() const noexcept { return spec_; }

 private:
  TextBoxSpec spec_;
  std::vector<std::string> ids_;  // 실행 뒤 줄별 객체 id
  bool executed_ = false;
};

}  // namespace onpdf::edit
