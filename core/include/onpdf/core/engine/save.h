#pragma once

// 안전 저장: 같은 폴더 임시 파일에 쓰고 → 다시 열어 검증 → 원자적으로 교체한다.
// 어느 단계에서 실패해도 대상(원본) 파일 바이트는 바뀌지 않고 임시 파일은 지워진다. 워커 스레드 전용.

#include "onpdf/core/error.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

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

// 생산자에 무관한 골격: 대상마다 같은 폴더 임시 파일을 만들고(쓰기 가능·권한 확인) write 가 모든 임시 파일을 채운 뒤,
// verify 가 파일마다 검사하고, 전부 통과하면 디스크 동기화 후 순서대로 교체한다. 하나라도 실패하면 어떤 대상도 바뀌지 않고
// 임시 파일은 지워진다. 스레드 무관 (호출자가 생산자의 스레드 규칙을 지킨다)
struct SafeWriteHooks {
  std::function<Result<void>(const std::vector<std::filesystem::path>& temps)> write;
  // index = targets 안의 위치. 비면 생략
  std::function<Result<void>(std::size_t index, const std::filesystem::path& target, const std::filesystem::path& temp)> verify;
};
// 여러 대상의 교체(rename) 가 중간에 실패하면 앞 대상은 이미 바뀐 채 남는다 — 같은 폴더 rename 이라 드물다
[[nodiscard]] Result<std::vector<SaveResult>> write_files_safely(const std::vector<std::filesystem::path>& targets,
                                                              const SafeWriteHooks& hooks);

}  // namespace onpdf::engine
