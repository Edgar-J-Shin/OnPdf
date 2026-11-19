#pragma once

// PDFium 헤더 없이 문서를 바꾸는 인터페이스. edit/ 의 Command 는 이것으로만 문서를 바꾼다.
// 구현(PdfiumDocument)은 워커 스레드에서만 쓴다.

#include "onpdf/core/engine/annotation.h"
#include "onpdf/core/engine/render_types.h"
#include "onpdf/core/engine/text_object.h"
#include "onpdf/core/error.h"

#include <filesystem>
#include <string_view>
#include <utility>
#include <vector>

namespace onpdf::engine {

// 편집 결과. 앱은 이것으로 문서 정보·화면·썸네일을 갱신한다
struct ChangeSet {
  std::vector<int> pages;                           // 내용·회전이 바뀐 쪽 (구조 변경이면 비움)
  std::vector<std::pair<int, PageSize>> new_sizes;  // 크기가 바뀐 쪽 (회전 포함, /Rotate 반영 크기)
  bool structural = false;                          // 쪽 수·순서가 바뀌었다. all_sizes 가 새 문서 전체의 쪽 크기
  std::vector<PageSize> all_sizes;
  int focus_page = -1;                              // 구조 변경 뒤 보여 줄 쪽 (옮긴 블록·삽입 자리·지운 자리). -1 = 현재 쪽 유지
  std::vector<int> selected_pages;                  // 구조 변경 뒤 썸네일에서 선택할 쪽 (옮긴·넣은·되살린 쪽). 비면 선택 없음
};

struct EditPermissions {
  bool modify_contents = true;  // 비트 4: 아래 셋이 다루지 않는 내용 수정
  bool annotate = true;         // 비트 6: 주석 추가·수정, 폼 채우기
  bool fill_forms = true;       // 비트 9(R≥3) 또는 비트 6: 폼 채우기·서명
  bool assemble = true;         // 쪽 삽입·회전·삭제, 목차: R≥3 은 비트 11, R2 는 비트 4
  bool extract = true;          // 비트 5: 복사·추출 — 다른 문서로 쪽을 가져가는 삽입 소스가 될 수 있는지
};

// 문서가 소유하는 보조 PDFium 문서(쪽 사본 또는 다른 파일)의 토큰. 0 은 없음.
// Command 는 이 값만 보관한다: 스냅샷 자체는 워커 스레드의 문서가 소유해 메인 스레드에서 소멸하지 않는다
using SnapshotId = int;

struct SnapshotInfo {
  int page_count = 0;
  EditPermissions permissions;
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

  // ---- 쪽 회전 ----
  [[nodiscard]] virtual Result<int> page_rotation(int page_index) const = 0;
  [[nodiscard]] virtual Result<void> set_page_rotation(int page_index, int quarter_turns) = 0;

  // ---- 구조 편집 ----
  // pages: 오름차순·중복 없는 쪽 번호. 전부 지우면 kInvalidArgument (문서에는 한 쪽 이상 남아야 한다).
  // 성공하면 쪽 수는 이전 − n. 실패(인자)하면 문서는 그대로다
  [[nodiscard]] virtual Result<void> delete_pages(const std::vector<int>& pages) = 0;
  // pages(중복 없음, 범위 안)를 한 블록으로 dest 위치(0 ≤ dest ≤ count − n: 블록을 뺀 뒤의 삽입 자리)로 옮긴다.
  // 호출 전에 인자를 검증하므로 kInvalidArgument 면 문서는 그대로다. 그 뒤의 실패는 kInvalidState
  [[nodiscard]] virtual Result<void> move_pages(const std::vector<int>& pages, int dest) = 0;
  // pages 의 사본을 담은 스냅샷을 만든다 (이 문서는 바뀌지 않는다). 삭제 취소용
  [[nodiscard]] virtual Result<SnapshotId> copy_pages_to_snapshot(const std::vector<int>& pages) = 0;
  // 다른 PDF 파일을 스냅샷으로 연다. 암호가 필요하면 kPasswordRequired
  [[nodiscard]] virtual Result<SnapshotId> open_snapshot(const std::filesystem::path& file,
                                                         std::string_view password) = 0;
  [[nodiscard]] virtual Result<SnapshotInfo> snapshot_info(SnapshotId id) const = 0;
  // 스냅샷의 src_pages(비어 있으면 전체)를 at(0 ≤ at ≤ count) 자리에 삽입한다. 성공하면 쪽 수는 이전 + n
  [[nodiscard]] virtual Result<void> insert_from_snapshot(SnapshotId id, const std::vector<int>& src_pages,
                                                          int at) = 0;
  // 스냅샷을 해제한다 (없는 id 는 무시)
  virtual void drop_snapshot(SnapshotId id) noexcept = 0;

  // ---- 주석 (id = /NM. NM 이 없는 기존 주석은 "#<index>" 로 가리키고, 편집하면 NM 이 부여된다) ----
  [[nodiscard]] virtual Result<std::vector<AnnotationInfo>> annotations(int page_index) const = 0;
  // 새 주석을 만든다 (AP 없음 — PDFium 이 렌더 때 만든다). 돌려주는 값은 /NM
  [[nodiscard]] virtual Result<std::string> add_annotation(int page_index, const AnnotationSpec& spec) = 0;
  // 되살리기용 원시값. NM 이 없으면 부여한다 (돌려준 snapshot.id 가 그 뒤의 id).
  // 주석의 AP 를 지운다 (색을 읽기 위해) — 삭제·색 변경 직전에만 부르고, 지워진 AP 는 다음 렌더가 다시 만든다
  [[nodiscard]] virtual Result<AnnotationSnapshot> snapshot_annotation(int page_index, std::string_view id) = 0;
  [[nodiscard]] virtual Result<void> remove_annotation(int page_index, std::string_view id) = 0;
  // 같은 NM 으로 다시 만든다 (모양은 PDFium 이 다시 그린다). /Annots 끝에 붙는다
  [[nodiscard]] virtual Result<void> restore_annotation(int page_index, const AnnotationSnapshot& snapshot) = 0;
  // AP 를 지우고 /C(+/CA) 를 쓴다. 다음 렌더가 새 AP 를 만든다
  [[nodiscard]] virtual Result<void> set_annotation_color(int page_index, std::string_view id, Color color) = 0;
  [[nodiscard]] virtual Result<void> set_annotation_contents(int page_index, std::string_view id,
                                                             std::string_view utf8) = 0;
  // AP 를 지우고 /Border 폭을 쓴다 (Ink·Square·Circle). /BS 가 있는 주석은 거절한다 — PDFium 은 /BS /W 를 우선하고
  // /BS 를 지울 API 가 없다 (Command 가 삭제·재생성으로 우회)
  [[nodiscard]] virtual Result<void> set_annotation_border_width(int page_index, std::string_view id, double width) = 0;

  // ---- 텍스트 객체 (쪽 콘텐츠, #45). 정체성은 마크 콘텐츠 "OnPdf" 의 id 파라미터 ----
  // 줄마다 텍스트 객체를 만들고 쪽 콘텐츠를 다시 생성한다. 폰트는 spec.font.key 로 문서가 캐시해 재사용한다.
  // 돌려주는 값 = 줄별 객체 id (spec.ids 가 있으면 그대로). 중간 실패 시 만든 객체를 지우고 오류 (문서 원상)
  [[nodiscard]] virtual Result<TextObjectsResult> add_text_objects(int page_index, const TextObjectsSpec& spec) = 0;
  // id 들을 전부 찾은 뒤에만 지운다 (하나라도 없으면 kInvalidArgument, 아무것도 지우지 않음)
  [[nodiscard]] virtual Result<void> remove_text_objects(int page_index, const std::vector<std::string>& ids) = 0;
  // OnPdf 가 만든 텍스트 객체 목록 (저장 검증·선택용)
  [[nodiscard]] virtual Result<std::vector<TextObjectInfo>> text_objects(int page_index) const = 0;
  // 텍스트 객체들을 쪽 포인트 delta 만큼 옮긴다 (행렬 e,f 만 변경, 회전 쪽에서도 화면 방향). 전부 찾은 뒤에만 바꾼다 (#47)
  [[nodiscard]] virtual Result<void> move_text_objects(int page_index, const std::vector<std::string>& ids, Point delta_pt) = 0;
};

}  // namespace onpdf::engine
