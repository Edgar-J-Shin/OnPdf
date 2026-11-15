#include "onpdf/core/engine/save.h"

#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/pdfium_library.h"
#include "onpdf/core/platform/fs_sync.h"
#include "onpdf/core/platform/path.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <random>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace onpdf::engine {
namespace {

// 같은 폴더(같은 볼륨)여야 교체가 원자적이다. 이름에는 64비트 난수를 넣어 프로세스(앱 두 개)끼리도 겹치지 않게 한다.
// 원래 파일 이름은 넣지 않는다 (긴 이름에서 파일 이름 길이 한도를 넘지 않게)
std::filesystem::path temp_path_in(const std::filesystem::path& dir) {
  std::random_device random;
  const std::uint64_t nonce = (static_cast<std::uint64_t>(random()) << 32) ^ static_cast<std::uint64_t>(random());
  char hex[17];
  std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(nonce));
  return dir / path_from_utf8(std::string(".onpdf-save-") + hex + ".tmp");
}

// 저장할 실제 파일: 심볼릭 링크면 링크가 가리키는 파일 (링크를 일반 파일로 바꾸지 않도록)
Result<std::filesystem::path> resolve_save_target(const std::filesystem::path& target) {
  std::error_code ec;
  if (!std::filesystem::is_symlink(std::filesystem::symlink_status(target, ec))) return target;
  std::filesystem::path real = std::filesystem::canonical(target, ec);
  if (ec) return make_error(ErrorCode::kIoError, "cannot resolve link " + path_to_utf8(target) + ": " + ec.message());
  return real;
}

// 실제로 쓸 수 있는지 (읽기 전용 속성·권한·잠금·다른 프로그램의 쓰기 금지 공유). 내용은 바꾸지 않는다
bool can_write_existing(const std::filesystem::path& file) {
  std::ofstream probe(file, std::ios::binary | std::ios::app);
  return static_cast<bool>(probe);
}

// 임시 파일을 지우는 RAII. 성공해 rename 되면 release()
class TempFile {
 public:
  explicit TempFile(std::filesystem::path path) : path_(std::move(path)) {}
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  ~TempFile() {
    if (!path_.empty()) {
      std::error_code ec;
      std::filesystem::remove(path_, ec);
    }
  }
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  void release() noexcept { path_.clear(); }

 private:
  std::filesystem::path path_;
};

Error verify_error(const std::string& what) { return Error{ErrorCode::kSaveVerifyFailed, what}; }

// 저장 결과 검증: 같은 암호로 열림, 쪽 수·크기 같음, 모든 쪽 로드, (증분이면) 원본 바이트가 앞부분과 같음
Result<void> verify_saved(const PdfiumDocument& source, const std::filesystem::path& saved, bool incremental) {
  auto reopened = PdfiumDocument::open(saved, source.password());
  if (!reopened) return std::unexpected(verify_error("cannot reopen saved file: " + reopened.error().message));
  const int count = source.page_count();
  if (reopened->page_count() != count) return std::unexpected(verify_error("page count changed"));
  for (int i = 0; i < count; ++i) {
    auto expected = source.page_size(i);
    auto actual = reopened->page_size(i);
    if (!expected || !actual) return std::unexpected(verify_error("cannot read page size " + std::to_string(i)));
    if (std::abs(expected->width - actual->width) > 0.01 || std::abs(expected->height - actual->height) > 0.01) {
      return std::unexpected(verify_error("page size changed on page " + std::to_string(i + 1)));
    }
    // 쪽을 실제로 열 수 있어야 한다 (회전 조회가 FPDF_LoadPage 를 거친다)
    if (auto loaded = reopened->page_rotation(i); !loaded) {
      return std::unexpected(verify_error("cannot load page " + std::to_string(i + 1)));
    }
  }
  if (incremental) {
    const auto& original = source.original_bytes();
    std::ifstream in(saved, std::ios::binary);
    std::vector<char> head(original.size());
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    if (static_cast<std::size_t>(in.gcount()) != original.size() ||
        !std::equal(original.begin(), original.end(), head.begin(),
                    [](std::uint8_t a, char b) { return a == static_cast<std::uint8_t>(b); })) {
      return std::unexpected(verify_error("incremental save did not preserve original bytes"));
    }
  }
  return {};
}

}  // namespace

Result<SaveResult> save_document_safely(const PdfiumDocument& doc, const std::filesystem::path& target,
                                        const SaveOptions& options) {
  PdfiumLibrary::assert_current_thread();
  if (target.empty() || !target.has_filename()) return make_error(ErrorCode::kInvalidArgument, "empty target path");
  auto resolved = resolve_save_target(target);
  if (!resolved) return std::unexpected(resolved.error());
  const std::filesystem::path real = std::move(*resolved);
  std::error_code ec;
  const std::filesystem::path dir = real.parent_path().empty() ? std::filesystem::path(".") : real.parent_path();
  if (!std::filesystem::is_directory(dir, ec)) {
    return make_error(ErrorCode::kIoError, "folder does not exist: " + path_to_utf8(dir));
  }

  // 기존 파일이면: 일반 파일이어야 하고, 쓸 수 있어야 하며(읽기 전용을 몰래 덮어쓰지 않는다), 권한을 그대로 옮긴다
  std::optional<std::filesystem::perms> keep_permissions;
  const auto status = std::filesystem::status(real, ec);
  if (!ec && std::filesystem::exists(status)) {
    if (!std::filesystem::is_regular_file(status)) {
      return make_error(ErrorCode::kInvalidArgument, "not a regular file: " + path_to_utf8(real));
    }
    if (!can_write_existing(real)) {
      return make_error(ErrorCode::kNotWritable, "cannot write " + path_to_utf8(real));
    }
    keep_permissions = status.permissions();
  }

  TempFile temp(temp_path_in(dir));
  if (auto written = doc.save_to(temp.path(), options.incremental); !written) return std::unexpected(written.error());
  if (options.after_write_for_testing) options.after_write_for_testing(temp.path());
  if (auto verified = verify_saved(doc, temp.path(), options.incremental); !verified) {
    return std::unexpected(verified.error());
  }
  if (keep_permissions) {
    // 새 파일은 기본 권한(umask)으로 만들어진다: 0600 문서가 0644 로 넓어지지 않게 원본 권한을 옮긴다.
    // 동기화(fsync) 전에 옮겨야 권한 변경도 함께 디스크에 남는다 (POSIX 동기화는 읽기 모드로 열어 권한과 무관)
    std::filesystem::permissions(temp.path(), *keep_permissions, std::filesystem::perm_options::replace, ec);
    if (ec) return make_error(ErrorCode::kIoError, "cannot copy permissions: " + ec.message());
  }

  std::error_code size_ec;
  const std::uintmax_t size = std::filesystem::file_size(temp.path(), size_ec);
  const std::uintmax_t bytes = size_ec ? 0 : size;
  // 교체 전에 내용을 디스크까지 내린다: 그렇지 않으면 교체 직후 전원이 꺼질 때 원본과 새 내용을 모두 잃을 수 있다
  if (auto flushed = platform::flush_file_to_disk(temp.path()); !flushed) return std::unexpected(flushed.error());
  if (auto replaced = platform::replace_file(temp.path(), real); !replaced) return std::unexpected(replaced.error());
  temp.release();
  // 이름 바꾸기 자체도 디스크에 남긴다. 이미 교체는 끝났으므로 실패해도 저장 실패로 보지 않는다
  (void)platform::flush_directory(dir);
  return SaveResult{target, options.incremental, bytes};
}

}  // namespace onpdf::engine
