#pragma once

// 안전 저장: 같은 폴더 임시 파일에 쓰고 → 다시 열어 검증 → 원자적으로 교체한다.
// 어느 단계에서 실패해도 대상(원본) 파일 바이트는 바뀌지 않고 임시 파일은 지워진다. 워커 스레드 전용.

#include "onpdf/core/error.h"

#include <cstdint>
#include <filesystem>
#include <functional>

namespace onpdf::engine {

class PdfiumDocument;

struct SaveOptions {
  bool incremental = false;  // 원본 바이트 뒤에 변경만 덧붙인다 (서명 문서는 호출자가 참으로)
  // 테스트용: 임시 파일을 쓴 뒤·검증 전에 호출된다 (검증 실패 주입)
  std::function<void(const std::filesystem::path& temp)> after_write_for_testing;
};

struct SaveResult {
  std::filesystem::path path;
  bool incremental = false;
  std::uintmax_t bytes = 0;
};

[[nodiscard]] Result<SaveResult> save_document_safely(const PdfiumDocument& doc, const std::filesystem::path& target,
                                                      const SaveOptions& options);

}  // namespace onpdf::engine
