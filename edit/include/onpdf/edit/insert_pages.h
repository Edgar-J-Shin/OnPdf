#pragma once

#include "onpdf/edit/command.h"

#include <filesystem>
#include <string>
#include <vector>

namespace onpdf::edit {

// 다른 PDF 파일의 쪽(source_pages, 비어 있으면 전체)을 at 자리에 삽입한다.
// 소스는 첫 실행 때 스냅샷으로 열어 Command 수명 동안 유지한다 (파일이 바뀌거나 사라져도 다시
// 실행이 된다). 소스가 추출을 금지하면(권한 비트 5) kPermissionDenied
class InsertPagesCommand final : public Command {
 public:
  InsertPagesCommand(std::filesystem::path source, std::string password,
                     std::vector<int> source_pages, int at);
  [[nodiscard]] std::string_view id() const noexcept override { return "insert_pages"; }
  Result<engine::ChangeSet> execute(engine::EditableDocument& doc) override;
  Result<engine::ChangeSet> undo(engine::EditableDocument& doc) override;
  void discard(engine::EditableDocument& doc) noexcept override;
  [[nodiscard]] int at() const noexcept { return at_; }
  [[nodiscard]] int inserted_count() const noexcept { return inserted_; }

 private:
  std::filesystem::path source_;
  std::string password_;
  std::vector<int> source_pages_;
  int at_;
  engine::SnapshotId snapshot_ = 0;  // 워커에서만 접근
  int inserted_ = 0;
};

}  // namespace onpdf::edit
