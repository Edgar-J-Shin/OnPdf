#pragma once

// 편집 Command 가 문서를 바꿀 때 쓰는 최소 인터페이스. PDFium 헤더에 의존하지 않으므로 edit/ 가 쓸 수 있다.
// 구현(PdfiumDocument)은 워커 스레드에서만 호출된다.

#include "onpdf/core/engine/render_types.h"
#include "onpdf/core/error.h"

#include <utility>
#include <vector>

namespace onpdf::engine {

// 편집이 바꾼 범위. 워커가 만들고 메인 스레드가 문서 정보·화면 갱신에 쓴다
struct ChangeSet {
  std::vector<int> pages;                           // 내용·회전이 바뀐 쪽
  std::vector<std::pair<int, PageSize>> new_sizes;  // 크기가 바뀐 쪽 (회전 포함, /Rotate 반영 크기)
};

class EditableDocument {
 public:
  EditableDocument() = default;
  EditableDocument(const EditableDocument&) = default;
  EditableDocument(EditableDocument&&) noexcept = default;
  EditableDocument& operator=(const EditableDocument&) = default;
  EditableDocument& operator=(EditableDocument&&) noexcept = default;
  virtual ~EditableDocument() = default;

  [[nodiscard]] virtual int page_count() const noexcept = 0;
  [[nodiscard]] virtual Result<PageSize> page_size(int page_index) const = 0;
  // 쪽 회전: 0..3 (시계 방향 90° 단위, /Rotate 값 / 90)
  [[nodiscard]] virtual Result<int> page_rotation(int page_index) const = 0;
  [[nodiscard]] virtual Result<void> set_page_rotation(int page_index, int quarter_turns) = 0;
};

}  // namespace onpdf::engine
