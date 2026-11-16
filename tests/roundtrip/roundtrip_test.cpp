// 왕복 테스트: manifest 의 열 수 있는 모든 PDF 를 열기 → (변경 없이) 안전 저장 → 다시 열기 후 일치 확인.
// 계획서 성공 기준 "테스트 PDF 열기·저장 왕복 후 깨지는 파일 0건". qpdf 명령이 있으면 qpdf --check 도 돌린다.

#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/save.h"

#include "../manifest.h"
#include "../test_support.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <string>

namespace onpdf::engine {
namespace {

using onpdf::test::load_manifest;
using onpdf::test::ManifestEntry;
using onpdf::test::ScopedTempDir;
using onpdf::test::test_name;
using onpdf::test::testdata;

// 렌더 결과 비교: 채널 차이가 허용치를 넘는 픽셀 비율
double different_fraction(const Bitmap& a, const Bitmap& b) {
  if (a.width != b.width || a.height != b.height) return 1.0;
  std::size_t diff = 0;
  for (int y = 0; y < a.height; ++y) {
    for (int x = 0; x < a.width; ++x) {
      const auto* pa = a.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(a.stride) +
                       static_cast<std::size_t>(x) * 4;
      const auto* pb = b.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(b.stride) +
                       static_cast<std::size_t>(x) * 4;
      for (int c = 0; c < 3; ++c) {
        if (std::abs(int{pa[c]} - int{pb[c]}) > onpdf::test::kChannelTolerance) {
          ++diff;
          break;
        }
      }
    }
  }
  return static_cast<double>(diff) / static_cast<double>(std::max(1, a.width * a.height));
}

// qpdf 명령 (ONPDF_QPDF 환경 변수 또는 PATH 의 qpdf). 없으면 빈 문자열
std::string qpdf_command(const std::filesystem::path& scratch) {
  static const std::string cached = [&] {
    const char* env = std::getenv("ONPDF_QPDF");
    const std::string exe = env != nullptr ? env : "qpdf";
    // 명령 맨 앞을 따옴표로 시작하지 않는다: Windows cmd 는 그 경우 첫·마지막 따옴표를 지워 명령이 깨진다
    // (ONPDF_QPDF 는 공백 없는 경로를 쓴다)
    const std::string probe = exe + " --version > \"" + onpdf::path_to_utf8(scratch / "qpdf-version.txt") + "\" 2>&1";
    return std::system(probe.c_str()) == 0 ? exe : std::string();
  }();
  return cached;
}

class RoundtripTest : public ::testing::TestWithParam<ManifestEntry> {};

TEST_P(RoundtripTest, SaveAndReopenMatches) {
  const ManifestEntry& e = GetParam();
  if (e.expect == "invalid") GTEST_SKIP() << "not openable by design";
  auto original = PdfiumDocument::open(testdata(e.file.c_str()), e.password);
  ASSERT_TRUE(original) << original.error().message;

  ScopedTempDir dir;
  const auto target = dir / "roundtrip.pdf";
  SaveOptions options;
  options.incremental = original->signature_count() > 0;
  auto saved = save_document_safely(*original, target, options);
  ASSERT_TRUE(saved) << e.file << ": " << saved.error().message;

  auto reopened = PdfiumDocument::open(target, e.password);
  ASSERT_TRUE(reopened) << reopened.error().message;
  ASSERT_EQ(reopened->page_count(), original->page_count());
  for (int i = 0; i < original->page_count(); ++i) {
    EXPECT_NEAR(reopened->page_size(i)->width, original->page_size(i)->width, 0.01) << "page " << i;
    EXPECT_NEAR(reopened->page_size(i)->height, original->page_size(i)->height, 0.01) << "page " << i;
  }
  if (original->page_count() > 0) {
    auto text_before = original->load_text(0);
    auto text_after = reopened->load_text(0);
    ASSERT_TRUE(text_before && text_after);
    EXPECT_EQ(text_after->char_count(), text_before->char_count());
    EXPECT_EQ(text_after->all_text(), text_before->all_text());
    RenderOptions render;
    render.scale = 0.5f;
    auto before = original->render_page(0, render);
    auto after = reopened->render_page(0, render);
    ASSERT_TRUE(before && after);
    EXPECT_LE(different_fraction(*before, *after), onpdf::test::kMaxDifferentFraction);
  }

  if (const std::string qpdf = qpdf_command(dir.path()); !qpdf.empty()) {
    std::string cmd = qpdf + " --check";
    if (!e.password.empty()) cmd += " --password=" + e.password;
    cmd += " \"" + onpdf::path_to_utf8(target) + "\" > \"" + onpdf::path_to_utf8(dir / "check.txt") + "\" 2>&1";
    const int status = std::system(cmd.c_str());
    // qpdf 종료 코드: 0 정상, 3 경고(원본에도 있던 경고 포함), 2 오류.
    // std::system 은 POSIX 에서 종료 코드를 8비트 민 대기 상태를, Windows 에서는 종료 코드 그대로를 돌려준다
    std::ifstream log(dir / "check.txt");
    const std::string output((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
    const bool ok = status == 0 || status == 3 || status == (3 << 8);
    EXPECT_TRUE(ok) << "qpdf --check status " << status << "\n" << output;
  }
}

INSTANTIATE_TEST_SUITE_P(Testdata, RoundtripTest, ::testing::ValuesIn(load_manifest()), test_name);

// 편집(쪽 회전) 후 저장한 문서도 왕복이 맞고, 원본 문서와는 다르다
TEST(Roundtrip, RotatedDocument) {
  auto doc = PdfiumDocument::open(testdata("bookmarks.pdf"));
  ASSERT_TRUE(doc);
  ASSERT_TRUE(doc->set_page_rotation(2, 1));
  ScopedTempDir dir;
  const auto target = dir / "rotated.pdf";
  ASSERT_TRUE(save_document_safely(*doc, target, {}));
  auto reopened = PdfiumDocument::open(target);
  ASSERT_TRUE(reopened);
  EXPECT_EQ(*reopened->page_rotation(2), 1);
  EXPECT_EQ(*reopened->page_rotation(1), 0);
  auto outline = reopened->load_outline();
  ASSERT_TRUE(outline);
  EXPECT_EQ(outline->size(), 15);  // 목차 보존
}

}  // namespace
}  // namespace onpdf::engine
