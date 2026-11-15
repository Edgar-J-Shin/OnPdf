#include "onpdf/core/engine/save.h"

#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/pdfium_library.h"
#include "onpdf/core/platform/path.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace onpdf::engine {
namespace {

std::filesystem::path temp_path_for(const std::filesystem::path& target) {
  static std::atomic<unsigned> counter{0};
  // 같은 폴더(같은 볼륨)여야 rename 이 원자적이다. 점으로 시작해 탐색기·Finder 에서 덜 보이게
  const std::string name = "." + path_to_utf8(target.filename()) + ".onpdf-" +
                           std::to_string(std::hash<std::string>{}(path_to_utf8(target)) % 100000) + "-" +
                           std::to_string(counter.fetch_add(1)) + ".tmp";
  return target.parent_path() / path_from_utf8(name);
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
  std::error_code ec;
  const std::filesystem::path dir = target.parent_path().empty() ? std::filesystem::path(".") : target.parent_path();
  if (!std::filesystem::is_directory(dir, ec)) {
    return make_error(ErrorCode::kIoError, "folder does not exist: " + path_to_utf8(dir));
  }

  TempFile temp(temp_path_for(target));
  if (auto written = doc.save_to(temp.path(), options.incremental); !written) return std::unexpected(written.error());
  if (options.after_write_for_testing) options.after_write_for_testing(temp.path());
  if (auto verified = verify_saved(doc, temp.path(), options.incremental); !verified) {
    return std::unexpected(verified.error());
  }

  std::error_code size_ec;
  const std::uintmax_t size = std::filesystem::file_size(temp.path(), size_ec);
  const std::uintmax_t bytes = size_ec ? 0 : size;
  // 원자적 교체: POSIX rename, MSVC STL 은 MoveFileExW(MOVEFILE_REPLACE_EXISTING). 같은 폴더라 같은 볼륨
  std::filesystem::rename(temp.path(), target, ec);
  if (ec) {
    return make_error(ErrorCode::kIoError, "cannot replace " + path_to_utf8(target) + ": " + ec.message());
  }
  temp.release();
  return SaveResult{target, options.incremental, bytes};
}

}  // namespace onpdf::engine
