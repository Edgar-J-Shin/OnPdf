#include "onpdf/core/platform/fs_sync.h"

#include "../test_support.h"
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace onpdf::platform {
namespace {

using onpdf::test::read_bytes;
using onpdf::test::ScopedTempDir;

void write_text(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
}

TEST(FsSync, FlushExistingFileAndFolder) {
  ScopedTempDir dir;
  const auto file = dir / "a.bin";
  write_text(file, "hello");
  EXPECT_TRUE(flush_file_to_disk(file));
  EXPECT_TRUE(flush_directory(dir.path()));
  auto missing = flush_file_to_disk(dir / "missing.bin");
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().code, ErrorCode::kFileNotFound);
}

TEST(FsSync, ReplaceOverwritesTargetAtomically) {
  ScopedTempDir dir;
  const auto from = dir / "new.bin";
  const auto to = dir / "한글 대상.bin";
  write_text(from, "new content");
  write_text(to, "old");
  ASSERT_TRUE(replace_file(from, to));
  EXPECT_FALSE(std::filesystem::exists(from));
  const auto bytes = read_bytes(to);
  EXPECT_EQ(std::string(bytes.begin(), bytes.end()), "new content");

  auto missing = replace_file(dir / "nope.bin", to);
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().code, ErrorCode::kFileNotFound);
}

}  // namespace
}  // namespace onpdf::platform
