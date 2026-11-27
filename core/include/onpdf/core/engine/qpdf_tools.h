#pragma once

// qpdf 기반 구조 작업(병합·쪽 추출·검사). qpdf 헤더는 qpdf_tools.cpp 안에서만 포함한다.
// PDFium 과 무관하므로 어느 스레드에서든 부를 수 있다 (한 번에 하나씩 — 호출자가 직렬화).
// 출력은 호출자가 write_files_safely 의 임시 파일을 넘겨 "임시 → 검증 → 교체" 를 지킨다

#include "onpdf/core/engine/render_hazard.h"
#include "onpdf/core/error.h"
#include "onpdf/core/page_ranges.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace onpdf::engine {

struct StructureInput {
  std::filesystem::path path;
  std::string password;  // UTF-8. 비면 없음
};

struct JobReport {
  std::vector<std::string> warnings;  // qpdf 경고 (손상 복구 등). 비어 있지 않아도 성공이다
  int converted_fonts = 0;            // rewrite 가 CFF 규격으로 바꾼 글꼴 수 (#73)
};

// rewrite 옵션 (#73): OnPdf 가 FPDFText_LoadCidType2Font 로 넣은 OTF(CFF) 서브셋은 /FontFile2 안의
// CFF 라 규격 위반이다. FontFile2 바이트의 FNV-1a 64 hex(= 엔진의 서브셋 key)가 convert_cff_keys 에
// 있으면 FontFile3/OpenType + CIDFontType0
// + charset 항등으로 바꾼다. 비어 있으면 재작성만 한다
struct RewriteOptions {
  std::vector<std::string> convert_cff_keys;
};

// 글꼴 통계 (테스트·검증용)
struct FontCensus {
  int cid_type2 = 0;
  int cid_type0 = 0;
  int fontfile2_otto = 0;  // /FontFile2 에 CFF(OTTO) — 규격 위반
  int fontfile3_opentype = 0;
  int cid_to_gid_maps = 0;
  bool charsets_identity = true;  // FontFile3/OpenType 의 CID-keyed CFF charset 이 모두 항등
};

using ProgressFn = std::function<void(int percent)>;  // 작업 스레드에서 호출된다

// 콘텐츠 스트림 수술 (#65): 쪽(과 그 쪽의 n 번째 Form XObject) 콘텐츠에서 "비어 있지 않은 텍스트
// 표시 연산자" (Tj/TJ/'/") 를 순번으로 골라 그 연산자만 바이트 단위로 지운다. 나머지 바이트는
// 그대로(PDFium 재생성 손실 없음). 순번 = 컨테이너 안 표시 연산자 순서 = PDFium 텍스트 객체 순서
// (호출자가 개수 일치를 검사한다)

// 새 객체(우리가 덧붙인 표시 연산자)에 자간을 넣는다 (#77): `<cs> Tc <피연산자> TJ 0 Tc`.
// char_space 는 텍스트 공간 단위(Tc). TJ 커닝이 아니라 Tc 인 이유: PDFium 텍스트 추출은 커닝 간격이
// 문턱을 넘으면 공백을 끼워 "C h a r" 가 되지만 Tc 는 기본 간격으로 본다(CalculateBaseSpace). 대신
// PDFium 재생성(그 객체를 옮기거나 크기를 바꿀 때)은 Tc 를 쓰지 않아 자간이 사라진다 → 엔진이 마크
// ls 로 다시 넣는다(#87). 그 연산자 시점의 유효 Tc 가 이미 char_space 면 건드리지 않는다(멱등)
struct ShowOpFit {
  int container =
      -1;  // -1 = 쪽만 적용한다 (Form 안의 새 객체는 만들지 않으므로 container ≥ 0 은 무시된다)
  int ordinal = 0;
  double char_space = 0.0;
};
struct ShowOpSurgery {
  std::set<int> page_ordinals;
  std::map<int, std::set<int>>
      form_ordinals;  // 쪽의 Form Do 순번(이미지 Do 제외) → 그 Form 안 순번
  std::vector<ShowOpFit> fits;
};
struct SurgeryReport {
  std::vector<std::uint8_t> bytes;
  int fitted = 0;                    // 자간(Tc)을 넣은 연산자 수
  int already_fitted = 0;            // 유효 Tc 가 이미 목표 값이라 건너뛴 fit 수 (#87)
  int page_show_ops = 0;             // 쪽 콘텐츠의 비어 있지 않은 표시 연산자 수
  std::map<int, int> form_show_ops;  // 수술한 Form 별
  int needs_displacement =
      0;  // 지운 연산자 뒤 같은 BT 의 표시 연산자가 위치 연산자 없이 이어진다 (밀림 — 거절)
  int shared_forms = 0;  // 수술 대상 Form 이 다른 쪽에서도 쓰인다 (거절)
};

class QpdfTools {
 public:
  // inputs[0] 이 주 입력: 그 문서의 목차·메타데이터·폼을 보존하고 나머지 입력의 쪽을 순서대로
  // 덧붙인다. 결과는 암호 없는 평문. 모든 입력은 추출이 허용돼야 한다(권한 비트 5,
  // kPermissionDenied). 암호가 틀리면 kPasswordRequired(메시지에 파일 이름)
  [[nodiscard]] static Result<JobReport> merge(const std::vector<StructureInput>& inputs,
                                               const std::filesystem::path& output,
                                               const ProgressFn& progress = {});
  // input 의 range 쪽만 output 으로 (분할의 한 조각)
  [[nodiscard]] static Result<JobReport> extract(const StructureInput& input, PageRange range,
                                                 const std::filesystem::path& output,
                                                 const ProgressFn& progress = {});
  // 구조 검사. 경고(손상 복구 등)는 성공으로 보고 report.warnings 에 담는다. 오류는 kJobFailed
  [[nodiscard]] static Result<JobReport> check(const std::filesystem::path& file,
                                               std::string_view password = {});
  // input 을 output 으로 다시 쓴다: 트레일러에서 닿는 객체만 남고(취소한 텍스트의 폰트 같은 미참조
  // 객체 폐기) 암호화·객체 스트림은 입력대로. 손상 복구 경고는 성공(report.warnings). 암호가 틀리면
  // kPasswordRequired. output 은 input 과 달라야 한다 (#61)
  [[nodiscard]] static Result<JobReport> rewrite(const std::filesystem::path& input,
                                                 const std::filesystem::path& output,
                                                 std::string_view password = {},
                                                 const RewriteOptions& options = {});
  [[nodiscard]] static Result<FontCensus> font_census(const std::filesystem::path& file,
                                                      std::string_view password = {});
  // 메모리 입출력. 암호화는 유지(password 로 연다). 실패는 kJobFailed, 쪽 범위 밖은
  // kInvalidArgument (#65)
  [[nodiscard]] static Result<SurgeryReport> remove_text_show_ops(
      const std::vector<std::uint8_t>& in, std::string_view password, int page_index,
      const ShowOpSurgery& ops);
  [[nodiscard]] static Result<int> page_count(const std::filesystem::path& file,
                                              std::string_view password = {});
  [[nodiscard]] static Result<bool> allows_extraction(const std::filesystem::path& file,
                                                      std::string_view password = {});
  // 쪽 /Annots 안에 직접(간접 참조가 아닌) 주석 사전이 몇 개인가. PDF 규격 표 30 은 간접 참조를
  // 요구한다 (#53 검증용)
  [[nodiscard]] static Result<int> direct_annotation_count(const std::filesystem::path& file,
                                                           std::string_view password = {});
  // 렌더 위험 검사 (#121): 쪽마다 /Resources(상속)·Form XObject·패턴 자신의 자원·주석 /AP 를 깊이 8,
  // 방문 집합으로 걷어 타일링 패턴(PatternType 1)의 쪽당 타일 수 ≈ 쪽 면적(MediaBox) /
  // (|XStep·YStep|·|det(Matrix)|) 를 추정한다. kMaxPatternTilesPerPage 를 넘거나 0·비유한이면
  // RenderHazard(쪽마다 가장 큰 것 하나). 열지 못하면 kPasswordRequired/kInvalidFormat — 호출자는
  // "위험 없음" 으로 두고 PDFium 에 맡긴다
  [[nodiscard]] static Result<std::vector<RenderHazard>> scan_render_hazards(
      const std::vector<std::uint8_t>& bytes, std::string_view password = {});
};

}  // namespace onpdf::engine
