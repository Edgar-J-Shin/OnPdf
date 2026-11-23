// Spike #55: qpdf 콘텐츠 스트림 수술 (PDFium 과 분리 — qpdf 헤더는 이 .cpp 에만)
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace spike {

struct SurgeryResult {
  bool ok = false;
  std::string error;
  int page_show_ops =
      0;  // 쪽 콘텐츠의 비어 있지 않은 표시 연산자 수 (PDFium 쪽 직속 텍스트 객체 수와 같아야 한다)
  int needs_displacement =
      0;  // 지운 연산자 뒤 같은 BT 의 표시 연산자가 위치 연산자 없이 이어짐(밀림 위험)
  int forms_rewritten = 0;
  std::vector<std::uint8_t> bytes;
};

// page_ordinals: 쪽 콘텐츠에서 지울 표시 연산자 순번. form_ordinals: 쪽의 n 번째 Form Do → 그
// 안에서 지울 순번
SurgeryResult remove_show_ops(const std::vector<std::uint8_t>& in, int page_index,
                              const std::set<int>& page_ordinals,
                              const std::map<int, std::set<int>>& form_ordinals);

}  // namespace spike
