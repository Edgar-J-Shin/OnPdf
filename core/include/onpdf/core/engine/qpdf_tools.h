#pragma once

// qpdf 기반 구조 작업(병합·쪽 추출·검사). qpdf 헤더는 qpdf_tools.cpp 안에서만 포함한다.
// PDFium 과 무관하므로 어느 스레드에서든 부를 수 있다 (한 번에 하나씩 — 호출자가 직렬화).
// 출력은 호출자가 write_files_safely 의 임시 파일을 넘겨 "임시 → 검증 → 교체" 를 지킨다

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
// (호출자가 개수 일치를 검사한다) 새 객체(우리가 덧붙인 `[<hex>] TJ`)에 균일 커닝을 넣는다 (#77):
// [<c1> k <c2> k … <cn>] TJ. kerning 은 TJ 숫자(1/1000 em, 양수 = 좁힘). code_count 로 코드
// 단위(1·2바이트)를 정한다
struct ShowOpFit {
  int container = -1;  // -1 = 쪽, n = 쪽의 n 번째 Form
  int ordinal = 0;
  double kerning = 0.0;
  int code_count = 0;
};
struct ShowOpSurgery {
  std::set<int> page_ordinals;
  std::map<int, std::set<int>>
      form_ordinals;  // 쪽의 Form Do 순번(이미지 Do 제외) → 그 Form 안 순번
  std::vector<ShowOpFit> fits;
};
struct SurgeryReport {
  std::vector<std::uint8_t> bytes;
  int fitted = 0;                    // 커닝을 넣은 연산자 수
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
};

}  // namespace onpdf::engine
