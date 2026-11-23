#pragma once

// 쪽 범위 문자열("1-3, 5, 8-")과 분할 규칙(N쪽마다·각 쪽), 출력 파일 이름 패턴. 순수 함수,
// Qt·PDFium 없음

#include "onpdf/core/error.h"

#include <string>
#include <string_view>
#include <vector>

namespace onpdf {

struct PageRange {
  int first = 1;  // 1 기준, 양끝 포함
  int last = 1;
  friend bool operator==(const PageRange&, const PageRange&) = default;
};

// "1-3, 5, 8-" → [{1,3},{5,5},{8,page_count}]. 공백 허용. 빈 항목·0·역순·범위 밖·숫자 아님은
// kInvalidArgument(메시지에 항목)
[[nodiscard]] Result<std::vector<PageRange>> parse_page_ranges(std::string_view text,
                                                               int page_count);
// n 쪽마다 (마지막 묶음은 짧을 수 있다). n ≤ 0 또는 page_count ≤ 0 이면 빈 목록
[[nodiscard]] std::vector<PageRange> ranges_every(int n, int page_count);
[[nodiscard]] std::vector<PageRange> ranges_each(int page_count);
// 출력 파일 이름. "%d" → 1 기준 순번(자릿수는 total 에 맞춰 0 채움), "%r" → "p001-003"(자릿수는
// range 끝에 맞춤). 둘 다 없으면 확장자 앞에 "-%d" 를 붙인 것처럼 처리한다
[[nodiscard]] std::string output_name(std::string_view pattern, int index, int total,
                                      PageRange range, int page_count);
// 범위 문자열 "a-b" (qpdf 문법)
[[nodiscard]] std::string range_text(PageRange range);

}  // namespace onpdf
