#pragma once

// 문서 전체에 걸친 텍스트 선택. 위치는 (쪽, 글자 경계 인덱스 [0, n]).
// 선택은 [start, end) 반열린 구간. 메인 스레드 전용 값 타입.

#include "onpdf/core/view/text_index.h"

#include <compare>
#include <functional>
#include <string>
#include <vector>

namespace onpdf::view {

struct TextPos {
  int page = -1;
  int index = 0;
  auto operator<=>(const TextPos&) const = default;
};

class TextSelection {
 public:
  [[nodiscard]] bool empty() const noexcept { return anchor_.page < 0 || anchor_ == focus_; }
  void set(TextPos anchor, TextPos focus) noexcept { anchor_ = anchor; focus_ = focus; }
  void set_focus(TextPos focus) noexcept { focus_ = focus; }
  void clear() noexcept { anchor_ = focus_ = TextPos{}; }

  [[nodiscard]] TextPos anchor() const noexcept { return anchor_; }
  [[nodiscard]] TextPos focus() const noexcept { return focus_; }
  [[nodiscard]] TextPos start() const noexcept { return anchor_ < focus_ ? anchor_ : focus_; }
  [[nodiscard]] TextPos end() const noexcept { return anchor_ < focus_ ? focus_ : anchor_; }

  // 그 쪽에서 선택된 글자 범위. 선택에 포함되지 않은 쪽은 빈 범위
  [[nodiscard]] TextRange range_on(int page, int page_char_count) const noexcept;
  [[nodiscard]] std::vector<int> pages() const;

  // 쪽별 인덱스 제공자로 텍스트를 합친다 (쪽 사이 "\n\n"). provider 가 nullptr 를 주면 그 쪽은 건너뛴다
  [[nodiscard]] std::string text(const std::function<const PageTextIndex*(int)>& provider) const;

 private:
  TextPos anchor_;
  TextPos focus_;
};

}  // namespace onpdf::view
