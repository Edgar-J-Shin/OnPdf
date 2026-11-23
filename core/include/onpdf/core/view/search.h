#pragma once

// 문서 검색. 쪽 검색 결과 타입, PDFium 범위 → 인덱스 범위 매핑, 그리고 메인 스레드용 SearchSession.
// SearchSession 은 워커 실행을 주입된 Executor 로 위임하므로 Qt·PDFium 에 의존하지 않는다.

#include "onpdf/core/engine/pdfium_worker.h"
#include "onpdf/core/error.h"
#include "onpdf/core/view/text_index.h"

#include <compare>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace onpdf::view {

struct SearchOptions {
  bool match_case = false;
  bool whole_word = false;
  auto operator<=>(const SearchOptions&) const = default;
};

// PDFium 글자 인덱스 범위 (줄바꿈 문자 포함 원본 인덱스)
struct SourceRange {
  int start = 0;
  int count = 0;
};

// 쪽 하나의 검색 결과. 쪽 인덱스 전체를 싣지 않고 결과별 하이라이트 사각형만 싣는다
// (흔한 검색어에 1,000쪽 문서여도 메모리가 결과 수에 비례하도록).
struct PageSearchResult {
  int page = -1;
  int char_count = 0;                    // 그 쪽 PDFium 글자 수 (텍스트 없는 문서 판정용)
  std::vector<TextRange> ranges;         // 텍스트 인덱스 기준, 오름차순, 비어 있지 않음
  std::vector<std::vector<Rect>> rects;  // ranges[i] 의 줄 단위 하이라이트 사각형 (쪽 포인트)
};

struct SearchHit {
  int page = -1;
  TextRange range;
  [[nodiscard]] bool operator==(const SearchHit&) const = default;
};

// PDFium 범위 → 인덱스 범위. 줄바꿈 문자만 덮으면 빈 범위
[[nodiscard]] TextRange map_source_range(const PageTextIndex& index, SourceRange source);

// 앞뒤 공백 제거, NBSP(U+00A0)·전각 공백(U+3000) → 공백. UTF-8
[[nodiscard]] std::string normalize_query(std::string_view utf8);

class SearchSession {
 public:
  using PageCallback = std::function<void(Result<PageSearchResult>)>;
  // 쪽 검색을 시작하고, 완료 시 on_done 을 **세션을 소유한 스레드(메인)** 에서 부른다
  using Executor = std::function<engine::TaskHandle(int page, const std::string& query,
                                                    SearchOptions options, PageCallback on_done)>;

  explicit SearchSession(Executor executor, int max_in_flight = 4, std::size_t max_hits = 100'000);
  ~SearchSession();
  SearchSession(const SearchSession&) = delete;
  SearchSession& operator=(const SearchSession&) = delete;

  // 이전 검색을 취소하고 새로 시작한다. 쿼리는 normalize_query 를 거친다. 빈 쿼리는 clear()
  void start(std::string query, SearchOptions options, int page_count, int start_page);
  void cancel();  // 진행 중 작업 취소, 결과 유지
  void clear();   // 취소 + 결과·쿼리 삭제

  [[nodiscard]] bool active() const noexcept { return !query_.empty(); }
  [[nodiscard]] const std::string& query() const noexcept { return query_; }
  [[nodiscard]] SearchOptions options() const noexcept { return options_; }
  [[nodiscard]] bool searching() const noexcept;
  [[nodiscard]] bool truncated() const noexcept { return truncated_; }
  // 검색이 끝났고 모든 쪽의 글자가 0 이면 true
  [[nodiscard]] bool no_text() const noexcept;
  [[nodiscard]] std::size_t total() const noexcept { return total_; }
  [[nodiscard]] int pages_done() const noexcept { return pages_done_; }
  [[nodiscard]] int page_count() const noexcept { return page_count_; }
  [[nodiscard]] int in_flight() const noexcept { return static_cast<int>(in_flight_.size()); }

  [[nodiscard]] std::optional<SearchHit> current() const noexcept { return current_; }
  // 현재 결과의 문서 순서 번호 (1부터). 없으면 0. 검색 중에는 앞쪽 미검색 쪽 때문에 바뀔 수 있다
  [[nodiscard]] std::size_t current_ordinal() const;

  // 순환 이동. 이동했으면 true. 검색 중 끝에 닿으면 이동하지 않고 false
  bool next();
  bool previous();
  [[nodiscard]] bool wrapped() const noexcept { return wrapped_; }

  [[nodiscard]] const std::vector<TextRange>* hits_on(int page) const;
  [[nodiscard]] const std::vector<std::vector<Rect>>* rects_on(int page) const;
  // 현재 결과의 하이라이트 사각형 (없으면 빈 벡터)
  [[nodiscard]] std::vector<Rect> current_rects() const;
  // 현재 결과가 사용자 이동(next/previous) 이 아니라 자동 선택(첫 결과 도착) 으로 정해졌는지
  [[nodiscard]] bool current_is_automatic() const noexcept { return current_automatic_; }

  std::function<void()> on_changed;          // 결과 수·진행률 변화
  std::function<void()> on_current_changed;  // 현재 결과 변경 (스크롤 대상)

  // 실행기 계약: on_done 은 동기·비동기 어느 쪽으로 불러도 된다. 동기 호출이어도 pump 는 재귀하지
  // 않는다.

 private:
  void pump();
  void on_page_done(int generation, int page, Result<PageSearchResult> result);
  void cancel_in_flight();
  void notify_changed();
  [[nodiscard]] std::vector<SearchHit> ordered_hits() const;

  Executor executor_;
  int max_in_flight_;
  std::size_t max_hits_;
  std::shared_ptr<int> alive_;  // weak_ptr 로 수명 확인

  std::string query_;
  SearchOptions options_;
  int generation_ = 0;
  int page_count_ = 0;
  int start_page_ = 0;
  std::vector<int> order_;
  std::size_t next_order_ = 0;
  std::unordered_map<int, engine::TaskHandle> in_flight_;
  std::map<int, PageSearchResult> results_;  // 문서 순서
  std::size_t total_ = 0;
  long long chars_seen_ = 0;
  int pages_done_ = 0;
  int pages_failed_ = 0;
  bool cancelled_ = false;
  bool in_pump_ = false;
  bool repump_ = false;
  bool current_automatic_ = false;
  bool truncated_ = false;
  bool wrapped_ = false;
  std::optional<SearchHit> current_;
};

}  // namespace onpdf::view
