#include "onpdf/core/view/search.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace onpdf::view {

TextRange map_source_range(const PageTextIndex& index, SourceRange source) {
  if (source.count <= 0 || index.chars.empty()) return {};
  const int s = source.start;
  const int e = source.start + source.count;  // 미포함
  auto first = std::lower_bound(index.chars.begin(), index.chars.end(), s,
                                [](const TextChar& c, int value) { return c.source_index < value; });
  auto last = std::lower_bound(index.chars.begin(), index.chars.end(), e,
                               [](const TextChar& c, int value) { return c.source_index < value; });
  const int a = static_cast<int>(first - index.chars.begin());
  const int b = static_cast<int>(last - index.chars.begin());
  return b > a ? TextRange{a, b} : TextRange{};
}

std::string normalize_query(std::string_view utf8) {
  std::string out;
  out.reserve(utf8.size());
  for (std::size_t i = 0; i < utf8.size(); ++i) {
    const auto c = static_cast<unsigned char>(utf8[i]);
    if (c == 0xC2 && i + 1 < utf8.size() && static_cast<unsigned char>(utf8[i + 1]) == 0xA0) {  // U+00A0
      out += ' ';
      ++i;
    } else if (c == 0xE3 && i + 2 < utf8.size() && static_cast<unsigned char>(utf8[i + 1]) == 0x80 &&
               static_cast<unsigned char>(utf8[i + 2]) == 0x80) {  // U+3000
      out += ' ';
      i += 2;
    } else if (c == '\t' || c == '\r' || c == '\n') {
      out += ' ';
    } else {
      out += static_cast<char>(c);
    }
  }
  const auto b = out.find_first_not_of(' ');
  if (b == std::string::npos) return {};
  const auto e = out.find_last_not_of(' ');
  return out.substr(b, e - b + 1);
}

// ---------------------------------------------------------------- SearchSession

SearchSession::SearchSession(Executor executor, int max_in_flight, std::size_t max_hits)
    : executor_(std::move(executor)),
      max_in_flight_(std::max(1, max_in_flight)),
      max_hits_(max_hits),
      alive_(std::make_shared<int>(0)) {}

SearchSession::~SearchSession() { cancel_in_flight(); }

void SearchSession::cancel_in_flight() {
  for (auto& [page, handle] : in_flight_) handle.cancel();
  in_flight_.clear();
}

void SearchSession::start(std::string query, SearchOptions options, int page_count, int start_page) {
  query = normalize_query(query);
  if (query.empty() || page_count <= 0) {
    clear();
    return;
  }
  cancel_in_flight();
  ++generation_;
  query_ = std::move(query);
  options_ = options;
  page_count_ = page_count;
  start_page_ = std::clamp(start_page, 0, page_count - 1);
  order_.clear();
  order_.reserve(static_cast<std::size_t>(page_count));
  for (int i = 0; i < page_count; ++i) order_.push_back((start_page_ + i) % page_count);
  next_order_ = 0;
  results_.clear();
  total_ = 0;
  chars_seen_ = 0;
  pages_done_ = 0;
  pages_failed_ = 0;
  cancelled_ = false;
  truncated_ = false;
  wrapped_ = false;
  current_automatic_ = false;
  current_.reset();
  notify_changed();
  if (on_current_changed) on_current_changed();
  pump();
}

void SearchSession::cancel() {
  cancel_in_flight();
  ++generation_;
  next_order_ = order_.size();  // 남은 쪽 발행 안 함
  cancelled_ = true;
  // 남은 결과 중 검색 순서상 첫 결과를 현재로 (대기 중이던 앞 쪽은 이제 오지 않는다)
  bool changed = false;
  if (!current_) {
    for (int p : order_) {
      if (auto it = results_.find(p); it != results_.end()) {
        current_ = SearchHit{p, it->second.ranges.front()};
        current_automatic_ = true;
        changed = true;
        break;
      }
    }
  }
  notify_changed();
  if (changed && on_current_changed) on_current_changed();
}

void SearchSession::clear() {
  cancel_in_flight();
  ++generation_;
  query_.clear();
  order_.clear();
  next_order_ = 0;
  results_.clear();
  total_ = 0;
  chars_seen_ = 0;
  pages_done_ = 0;
  pages_failed_ = 0;
  page_count_ = 0;
  cancelled_ = false;
  truncated_ = false;
  wrapped_ = false;
  current_automatic_ = false;
  current_.reset();
  notify_changed();
  if (on_current_changed) on_current_changed();
}

bool SearchSession::searching() const noexcept {
  return active() && !truncated_ && (next_order_ < order_.size() || !in_flight_.empty());
}

bool SearchSession::no_text() const noexcept {
  // 모든 쪽을 실제로 검색했고(취소·실패 없이) 글자가 하나도 없을 때만
  return active() && !searching() && !cancelled_ && !truncated_ && pages_failed_ == 0 &&
         pages_done_ == page_count_ && chars_seen_ == 0;
}

void SearchSession::notify_changed() {
  if (on_changed) on_changed();
}

void SearchSession::pump() {
  // 비재귀: 동기 실행기가 executor_ 안에서 on_page_done → pump 를 부르면 표시만 하고 바깥 루프가 이어 간다
  if (in_pump_) {
    repump_ = true;
    return;
  }
  in_pump_ = true;
  std::weak_ptr<int> alive = alive_;
  do {
    repump_ = false;
    while (next_order_ < order_.size() && static_cast<int>(in_flight_.size()) < max_in_flight_ && !truncated_) {
      const int page = order_[next_order_++];
      const int generation = generation_;
      // 동기 실행기는 executor_ 안에서 바로 콜백하므로, 핸들 자리를 먼저 만든다
      in_flight_.emplace(page, engine::TaskHandle{});
      engine::TaskHandle handle =
          executor_(page, query_, options_, [this, alive, generation, page](Result<PageSearchResult> r) {
            if (alive.expired()) return;
            on_page_done(generation, page, std::move(r));
          });
      if (alive.expired()) return;  // 콜백 안에서 세션이 사라졌다
      // 그 사이 같은 쪽의 새 요청(재진입 start)이 생겼다면 덮어쓰지 않는다
      if (generation == generation_) {
        if (auto it = in_flight_.find(page); it != in_flight_.end()) it->second = std::move(handle);
      } else {
        handle.cancel();
      }
    }
  } while (repump_);
  in_pump_ = false;
}

void SearchSession::on_page_done(int generation, int page, Result<PageSearchResult> result) {
  if (generation != generation_) return;  // 이전 검색
  in_flight_.erase(page);
  ++pages_done_;
  PageSearchResult r;
  r.page = page;
  if (result) {
    r = std::move(*result);
    r.page = page;
  } else {
    ++pages_failed_;
    if (result.error().code != ErrorCode::kCancelled && result.error().code != ErrorCode::kInvalidState) {
      std::fprintf(stderr, "[onpdf] search failed for page %d: %s\n", page, result.error().message.c_str());
    }
  }
  chars_seen_ += r.char_count;
  if (!r.ranges.empty()) {
    if (total_ + r.ranges.size() > max_hits_) {
      r.ranges.resize(max_hits_ - total_);
      if (r.rects.size() > r.ranges.size()) r.rects.resize(r.ranges.size());
      truncated_ = true;
    }
    total_ += r.ranges.size();
    if (!r.ranges.empty()) results_[page] = std::move(r);
  }
  if (truncated_) {
    cancel_in_flight();
    next_order_ = order_.size();  // 남은 쪽 발행 안 함
  }

  // 현재 결과가 없으면 "시작 쪽 이후(문서 순서상 순환) 첫 결과" 를 현재로
  bool current_changed = false;
  if (!current_ && !results_.empty()) {
    // 시작 쪽 이후에서 가장 앞선 결과. 단, 그보다 앞선 쪽이 아직 검색 중이면 기다린다
    for (int i = 0; i < static_cast<int>(order_.size()); ++i) {
      const int p = order_[static_cast<std::size_t>(i)];
      if (auto it = results_.find(p); it != results_.end()) {
        current_ = SearchHit{p, it->second.ranges.front()};
        current_automatic_ = true;
        current_changed = true;
        break;
      }
      // 이 쪽이 아직 끝나지 않았으면(발행 전이거나 진행 중) 더 뒤 결과를 현재로 고르지 않는다
      const bool done = static_cast<std::size_t>(i) < next_order_ && !in_flight_.contains(p);
      if (!done) break;
    }
  }
  std::weak_ptr<int> alive = alive_;
  notify_changed();
  if (alive.expired()) return;  // 콜백이 세션을 없앴다
  if (current_changed && on_current_changed) on_current_changed();
  if (alive.expired()) return;
  pump();
}

std::vector<SearchHit> SearchSession::ordered_hits() const {
  std::vector<SearchHit> out;
  out.reserve(total_);
  for (const auto& [page, r] : results_) {
    for (const auto& range : r.ranges) out.push_back({page, range});
  }
  return out;
}

std::size_t SearchSession::current_ordinal() const {
  if (!current_) return 0;
  std::size_t n = 0;
  for (const auto& [page, r] : results_) {
    if (page < current_->page) {
      n += r.ranges.size();
      continue;
    }
    if (page == current_->page) {
      for (const auto& range : r.ranges) {
        ++n;
        if (range == current_->range) return n;
      }
    }
    break;
  }
  return n;
}

bool SearchSession::next() {
  wrapped_ = false;
  if (total_ == 0) return false;
  const auto hits = ordered_hits();
  if (!current_) {
    // 아직 현재가 없으면(시작 쪽 대기 중) 검색 순서상 첫 결과
    for (int p : order_) {
      if (auto it = results_.find(p); it != results_.end()) {
        current_ = SearchHit{p, it->second.ranges.front()};
        break;
      }
    }
  } else {
    auto it = std::upper_bound(hits.begin(), hits.end(), *current_, [](const SearchHit& a, const SearchHit& b) {
      return a.page != b.page ? a.page < b.page : a.range.start < b.range.start;
    });
    if (it == hits.end()) {
      if (searching()) return false;  // 아직 검색되지 않은 쪽이 있을 수 있음
      current_ = hits.front();
      wrapped_ = true;
    } else {
      current_ = *it;
    }
  }
  current_automatic_ = false;
  if (on_current_changed) on_current_changed();
  return true;
}

bool SearchSession::previous() {
  wrapped_ = false;
  if (total_ == 0) return false;
  const auto hits = ordered_hits();
  if (!current_) {
    current_ = hits.back();
  } else {
    auto it = std::lower_bound(hits.begin(), hits.end(), *current_, [](const SearchHit& a, const SearchHit& b) {
      return a.page != b.page ? a.page < b.page : a.range.start < b.range.start;
    });
    if (it == hits.begin()) {
      if (searching()) return false;
      current_ = hits.back();
      wrapped_ = true;
    } else {
      current_ = *std::prev(it);
    }
  }
  current_automatic_ = false;
  if (on_current_changed) on_current_changed();
  return true;
}

const std::vector<TextRange>* SearchSession::hits_on(int page) const {
  auto it = results_.find(page);
  return it == results_.end() ? nullptr : &it->second.ranges;
}

const std::vector<std::vector<Rect>>* SearchSession::rects_on(int page) const {
  auto it = results_.find(page);
  return it == results_.end() ? nullptr : &it->second.rects;
}

std::vector<Rect> SearchSession::current_rects() const {
  if (!current_) return {};
  auto it = results_.find(current_->page);
  if (it == results_.end()) return {};
  const auto& r = it->second;
  for (std::size_t i = 0; i < r.ranges.size() && i < r.rects.size(); ++i) {
    if (r.ranges[i] == current_->range) return r.rects[i];
  }
  return {};
}

}  // namespace onpdf::view
