#pragma once

// 문서 목차(Outline/북마크) 불변 스냅샷. 워커가 만들고 메인 스레드가 shared_ptr<const> 로 읽는다.
// 항목은 트리 전위 순서로 평탄화되어 있다.

#include "onpdf/core/view/text_index.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace onpdf::view {

struct OutlineItem {
  std::string title;            // UTF-8. 비어 있으면 UI 가 "(제목 없음)"
  std::optional<Link> target;   // kPage(page + user_location) / kUri. 대상 없음·미지원 액션은 nullopt
  bool open = false;            // /Count > 0 (펼친 상태)
  int depth = 0;
  int parent = -1;              // items 인덱스
  std::vector<int> children;    // items 인덱스
  double approx_y = 0.0;        // 대상 위치의 근사 쪽 포인트 y (현재 항목 판정용, 쪽을 열지 않고 계산)
};

struct Outline {
  static constexpr int kMaxDepth = 32;
  static constexpr int kMaxItems = 100'000;

  std::vector<OutlineItem> items;  // 전위 순서
  std::vector<int> roots;
  bool truncated = false;          // 깊이·노드 수 제한에 걸림
  bool cycle_detected = false;     // 순환 참조를 끊음

  [[nodiscard]] bool empty() const noexcept { return items.empty(); }
  [[nodiscard]] int size() const noexcept { return static_cast<int>(items.size()); }

  // 대상 위치가 (page, y_pt) 이하인 마지막 항목 (문서 위치 기준). 같은 위치면 더 깊은(뒤) 항목. 없으면 -1
  [[nodiscard]] int item_at(int page, double y_pt) const;
  // item_at 용 정렬 색인. 항목을 채운 뒤 한 번 호출
  void build_position_index();

 private:
  struct Key {
    int page;
    double y;
    int index;
  };
  std::vector<Key> by_position_;
};

}  // namespace onpdf::view
