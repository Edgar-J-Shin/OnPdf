#include "onpdf/core/engine/save.h"

#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/pdfium_library.h"
#include "onpdf/core/platform/fs_sync.h"
#include "onpdf/core/platform/path.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <random>
#include <fstream>
#include <set>
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
    // OnPdf 가 마크한 쪽 객체(텍스트·이미지 id)가 저장 뒤에도 같은 집합이어야 한다 (#45·#49)
    auto before_objects = source.page_objects(i);
    auto after_objects = reopened->page_objects(i);
    if (!before_objects || !after_objects) return std::unexpected(verify_error("cannot list page objects on page " + std::to_string(i + 1)));
    std::set<std::string> before_ids, after_ids;
    for (const auto& o : *before_objects) if (o.marked) before_ids.insert(o.id);
    for (const auto& o : *after_objects) if (o.marked) after_ids.insert(o.id);
    if (before_ids != after_ids) return std::unexpected(verify_error("page objects changed on page " + std::to_string(i + 1)));
  }
  // 폼 필드 값이 저장 뒤에도 같아야 한다 (#51)
  {
    auto before_values = source.form_values();
    auto after_values = reopened->form_values();
    if (!before_values || !after_values) return std::unexpected(verify_error("cannot read form field values"));
    if (*before_values != *after_values) return std::unexpected(verify_error("form field values changed"));
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

Result<std::vector<SaveResult>> write_files_safely(const std::vector<std::filesystem::path>& targets,
                                                   const SafeWriteHooks& hooks) {
  if (targets.empty()) return make_error(ErrorCode::kInvalidArgument, "no target paths");
  if (!hooks.write) return make_error(ErrorCode::kInvalidArgument, "no writer");
  struct Prepared {
    std::filesystem::path real;
    std::filesystem::path dir;
    std::filesystem::path normalized;
    std::optional<std::filesystem::perms> keep_permissions;
  };
  std::vector<Prepared> prepared;
  std::vector<std::filesystem::path> prepared_normalized;
  std::vector<std::unique_ptr<TempFile>> temps;
  std::vector<std::filesystem::path> temp_paths;
  for (const auto& target : targets) {
    if (target.empty() || !target.has_filename()) return make_error(ErrorCode::kInvalidArgument, "empty target path");
    {
      // 같은 파일을 두 번 쓰지 않는다. 아직 없는 파일도 비교해야 하므로 정규화한 절대 경로로 (리뷰 I3)
      std::error_code norm_ec;
      const auto normalized = std::filesystem::weakly_canonical(std::filesystem::absolute(target, norm_ec), norm_ec).lexically_normal();
      for (const auto& other : prepared) {
        if (other.normalized == normalized) return make_error(ErrorCode::kInvalidArgument, "duplicate target path: " + path_to_utf8(target));
      }
      prepared_normalized.push_back(normalized);
    }
    auto resolved = resolve_save_target(target);
    if (!resolved) return std::unexpected(resolved.error());
    Prepared p;
    p.real = std::move(*resolved);
    p.normalized = prepared_normalized.back();
    std::error_code ec;
    p.dir = p.real.parent_path().empty() ? std::filesystem::path(".") : p.real.parent_path();
    if (!std::filesystem::is_directory(p.dir, ec)) {
      return make_error(ErrorCode::kIoError, "folder does not exist: " + path_to_utf8(p.dir));
    }
    // 기존 파일이면: 일반 파일이어야 하고, 쓸 수 있어야 하며(읽기 전용을 몰래 덮어쓰지 않는다), 권한을 그대로 옮긴다
    const auto status = std::filesystem::status(p.real, ec);
    if (!ec && std::filesystem::exists(status)) {
      if (!std::filesystem::is_regular_file(status)) {
        return make_error(ErrorCode::kInvalidArgument, "not a regular file: " + path_to_utf8(p.real));
      }
      if (!can_write_existing(p.real)) return make_error(ErrorCode::kNotWritable, "cannot write " + path_to_utf8(p.real));
      p.keep_permissions = status.permissions();
    }
    auto temp = std::make_unique<TempFile>(temp_path_in(p.dir));
    // 파일은 쓸 수 있어도 폴더에 새 파일을 만들 수 없는 경우(일부 공유·클라우드 폴더)를 따로 알린다
    if (std::ofstream probe(temp->path(), std::ios::binary | std::ios::trunc); !probe) {
      return make_error(ErrorCode::kFolderNotWritable, "cannot create files in folder " + path_to_utf8(p.dir));
    }
    temp_paths.push_back(temp->path());
    temps.push_back(std::move(temp));
    prepared.push_back(std::move(p));
  }

  if (auto written = hooks.write(temp_paths); !written) return std::unexpected(written.error());
  std::vector<SaveResult> results;
  for (std::size_t i = 0; i < targets.size(); ++i) {
    if (hooks.verify) {
      if (auto verified = hooks.verify(i, targets[i], temp_paths[i]); !verified) return std::unexpected(verified.error());
    }
    std::error_code ec;
    if (prepared[i].keep_permissions) {
      // 새 파일은 기본 권한(umask)으로 만들어진다: 0600 문서가 0644 로 넓어지지 않게 원본 권한을 옮긴다.
      // 동기화(fsync) 전에 옮겨야 권한 변경도 함께 디스크에 남는다 (POSIX 동기화는 읽기 모드로 열어 권한과 무관)
      std::filesystem::permissions(temp_paths[i], *prepared[i].keep_permissions, std::filesystem::perm_options::replace, ec);
      if (ec) return make_error(ErrorCode::kIoError, "cannot copy permissions: " + ec.message());
    }
    std::error_code size_ec;
    const std::uintmax_t size = std::filesystem::file_size(temp_paths[i], size_ec);
    // 교체 전에 내용을 디스크까지 내린다: 그렇지 않으면 교체 직후 전원이 꺼질 때 원본과 새 내용을 모두 잃을 수 있다
    if (auto flushed = platform::flush_file_to_disk(temp_paths[i]); !flushed) return std::unexpected(flushed.error());
    results.push_back(SaveResult{targets[i], false, size_ec ? 0 : size});
  }
  // 전부 검증됐다: 순서대로 교체. (여러 파일일 때 중간 실패는 이미 바뀐 파일을 되돌리지 않는다 — 같은 폴더 rename 이라 드물다)
  for (std::size_t i = 0; i < targets.size(); ++i) {
    if (auto replaced = platform::replace_file(temp_paths[i], prepared[i].real); !replaced) return std::unexpected(replaced.error());
    temps[i]->release();
  }
  for (const auto& p : prepared) (void)platform::flush_directory(p.dir);  // 이미 교체는 끝났으므로 실패해도 저장 실패로 보지 않는다
  return results;
}

Result<SaveResult> save_document_safely(const PdfiumDocument& doc, const std::filesystem::path& target,
                                        const SaveOptions& options) {
  PdfiumLibrary::assert_current_thread();
  SafeWriteHooks hooks;
  hooks.write = [&](const std::vector<std::filesystem::path>& temps) -> Result<void> {
    if (auto written = doc.save_to(temps.front(), options.incremental); !written) return written;
    if (options.after_write_for_testing) options.after_write_for_testing(temps.front());
    return {};
  };
  hooks.verify = [&](std::size_t, const std::filesystem::path&, const std::filesystem::path& temp) {
    return verify_saved(doc, temp, options.incremental);
  };
  auto results = write_files_safely({target}, hooks);
  if (!results) return std::unexpected(results.error());
  SaveResult result = results->front();
  result.incremental = options.incremental;
  return result;
}

}  // namespace onpdf::engine
