#pragma once

// qpdf 기반 구조 작업(병합·쪽 추출·검사). qpdf 헤더는 qpdf_tools.cpp 안에서만 포함한다.
// PDFium 과 무관하므로 어느 스레드에서든 부를 수 있다 (한 번에 하나씩 — 호출자가 직렬화).
// 출력은 호출자가 write_files_safely 의 임시 파일을 넘겨 "임시 → 검증 → 교체" 를 지킨다

#include "onpdf/core/error.h"
#include "onpdf/core/page_ranges.h"

#include <filesystem>
#include <functional>
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
};

using ProgressFn = std::function<void(int percent)>;  // 작업 스레드에서 호출된다

class QpdfTools {
 public:
  // inputs[0] 이 주 입력: 그 문서의 목차·메타데이터·폼을 보존하고 나머지 입력의 쪽을 순서대로 덧붙인다. 결과는 암호 없는 평문.
  // 모든 입력은 추출이 허용돼야 한다(권한 비트 5, kPermissionDenied). 암호가 틀리면 kPasswordRequired(메시지에 파일 이름)
  [[nodiscard]] static Result<JobReport> merge(const std::vector<StructureInput>& inputs, const std::filesystem::path& output,
                                               const ProgressFn& progress = {});
  // input 의 range 쪽만 output 으로 (분할의 한 조각)
  [[nodiscard]] static Result<JobReport> extract(const StructureInput& input, PageRange range,
                                                 const std::filesystem::path& output, const ProgressFn& progress = {});
  // 구조 검사. 경고(손상 복구 등)는 성공으로 보고 report.warnings 에 담는다. 오류는 kJobFailed
  [[nodiscard]] static Result<JobReport> check(const std::filesystem::path& file, std::string_view password = {});
  // input 을 output 으로 다시 쓴다: 트레일러에서 닿는 객체만 남고(취소한 텍스트의 폰트 같은 미참조 객체 폐기) 암호화·객체
  // 스트림은 입력대로. 손상 복구 경고는 성공(report.warnings). 암호가 틀리면 kPasswordRequired. output 은 input 과 달라야 한다 (#61)
  [[nodiscard]] static Result<JobReport> rewrite(const std::filesystem::path& input, const std::filesystem::path& output,
                                                 std::string_view password = {});
  [[nodiscard]] static Result<int> page_count(const std::filesystem::path& file, std::string_view password = {});
  [[nodiscard]] static Result<bool> allows_extraction(const std::filesystem::path& file, std::string_view password = {});
  // 쪽 /Annots 안에 직접(간접 참조가 아닌) 주석 사전이 몇 개인가. PDF 규격 표 30 은 간접 참조를 요구한다 (#53 검증용)
  [[nodiscard]] static Result<int> direct_annotation_count(const std::filesystem::path& file, std::string_view password = {});
};

}  // namespace onpdf::engine
