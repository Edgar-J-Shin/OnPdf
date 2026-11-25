#pragma once

// 쪽 구조 편집 공용 유틸: 순열 계산과 적용, 구조 변경 ChangeSet. 순수 함수라 가짜 문서로 테스트한다

#include "onpdf/core/engine/editable_document.h"
#include "onpdf/core/error.h"

#include <vector>

namespace onpdf::edit {

// 정렬하고 중복을 없앤다
[[nodiscard]] std::vector<int> normalize_pages(std::vector<int> pages);

// move_pages 인자 검증: 비어 있지 않음, 범위 안, 중복 없음, 0 ≤ dest ≤ count − n
[[nodiscard]] Result<void> validate_move(int count, const std::vector<int>& pages, int dest);

// pages 블록을 dest 로 옮긴 뒤의 순서. 결과[new_index] = old_index (FPDF_MovePages 와 같은 의미)
[[nodiscard]] std::vector<int> simulate_move(int count, const std::vector<int>& pages, int dest);

// order[new] = old → inv[old] = new
[[nodiscard]] std::vector<int> invert_order(const std::vector<int>& order);

// 현재 문서를 target 순서로 재배열한다: target[new] = 현재 인덱스. 단일 쪽 move_pages 의 열(최대 n
// 회)
[[nodiscard]] Result<void> apply_order(engine::EditableDocument& doc,
                                       const std::vector<int>& target);

// 쪽 수·순서가 바뀐 뒤의 ChangeSet (all_sizes 에 전체 쪽 크기). focus_page 는 새 범위로 클램프한다
[[nodiscard]] Result<engine::ChangeSet> structural_change(const engine::EditableDocument& doc,
                                                          int focus_page = -1,
                                                          std::vector<int> selected_pages = {});

}  // namespace onpdf::edit
