#pragma once

// 쪽 하나의 텍스트 인덱스(불변 스냅샷). 워커가 만들고 메인 스레드가 shared_ptr<const> 로 읽는다.
// 모든 좌표는 "쪽 포인트" — 좌상단 원점, y 아래, /Rotate·CropBox 반영. PageLayout::page_px_to_pt 와 같은 좌표계.

#include "onpdf/core/geometry.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace onpdf::view {

struct TextRange {
  int start = 0;  // 포함
  int end = 0;    // 미포함
  [[nodiscard]] bool empty() const noexcept { return end <= start; }
  [[nodiscard]] bool operator==(const TextRange&) const = default;
};

struct TextChar {
  char32_t code = 0;    // 유니코드. 매핑 실패는 U+FFFD
  int source_index = -1;  // PDFium 글자 인덱스 (줄바꿈 문자를 버리므로 어긋남 — 검색 결과 매핑용)
  Rect box;             // 생성 공백 등 상자가 없는 글자는 empty()
  double baseline_y = 0.0;
  float font_size = 0.0f;
  bool generated = false;  // PDFium 이 레이아웃 분석으로 삽입한 공백
};

struct TextLine {
  TextRange chars;  // 연속 범위
  Rect box;         // 상자 있는 글자들의 합집합 (전부 생성 공백이면 empty)
};

enum class LinkKind { kPage, kUri };

struct Link {
  Rect box;
  LinkKind kind = LinkKind::kPage;
  int page = -1;                   // kPage
  std::optional<Point> location;   // kPage: /XYZ 위치(대상 쪽 포인트). 없으면 쪽 상단
  std::string uri;                 // kUri, UTF-8
};

// 외부에서 열어도 되는 URI 인지 (http, https, mailto 만)
[[nodiscard]] bool is_safe_uri(std::string_view uri) noexcept;

// 단어 경계로 취급하는 글자 (공백·구두점)
[[nodiscard]] bool is_word_separator(char32_t code) noexcept;

void append_utf8(std::string& out, char32_t code);

class PageTextIndex {
 public:
  int page = -1;
  std::vector<TextChar> chars;
  std::vector<TextLine> lines;   // chars.start 오름차순, 서로 겹치지 않음, 빈 범위 없음
  std::vector<Link> links;

  [[nodiscard]] int char_count() const noexcept { return static_cast<int>(chars.size()); }
  [[nodiscard]] bool empty() const noexcept { return chars.empty(); }

  // pt 에서 tolerance(포인트) 안에 상자가 있는 글자
  [[nodiscard]] std::optional<int> hit_test(Point pt, double tolerance = 2.0) const;
  // 가장 가까운 선택 위치 [0, n]. 글자가 없으면 0
  [[nodiscard]] int nearest_position(Point pt) const;
  [[nodiscard]] int line_of(int ch) const;             // 없으면 -1
  [[nodiscard]] TextRange word_at(int ch) const;       // 구분자는 그 글자 하나
  [[nodiscard]] TextRange line_at(int ch) const;
  // 줄 단위 사각형(선택 하이라이트용). 빈 줄·상자 없는 글자만 있는 줄은 생략
  [[nodiscard]] std::vector<Rect> selection_rects(TextRange range) const;
  // UTF-8. 줄 경계마다 '\n'
  [[nodiscard]] std::string text(TextRange range) const;
  [[nodiscard]] std::string all_text() const { return text({0, char_count()}); }
  [[nodiscard]] const Link* link_at(Point pt) const noexcept;
};

}  // namespace onpdf::view
