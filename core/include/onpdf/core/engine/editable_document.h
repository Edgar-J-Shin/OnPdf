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

// 문서 보안 설정의 편집 권한 (PDF 32000-1 표 22). 암호화되지 않았거나 소유자 암호로 열었으면 모두 참.
// 앱은 편집 종류마다 해당 권한으로 판정한다 (쪽 회전·삭제·삽입 = assemble, 주석 = annotate)
struct EditPermissions {
  bool modify_contents = true;  // 비트 4: 아래 셋이 다루지 않는 내용 수정
  bool annotate = true;         // 비트 6: 주석 추가·수정, 폼 채우기
  bool fill_forms = true;       // 비트 9(R≥3) 또는 비트 6: 폼 채우기·서명
  bool assemble = true;         // 쪽 삽입·회전·삭제, 목차: R≥3 은 비트 11, R2 는 비트 4
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
