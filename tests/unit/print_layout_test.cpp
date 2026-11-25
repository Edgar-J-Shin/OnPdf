#include "onpdf/core/view/print_layout.h"

#include "onpdf/core/engine/pdfium_document.h"

#include "../test_support.h"
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <utility>
#include <vector>

namespace onpdf::view {
namespace {

using engine::PageSize;
using engine::PdfiumDocument;
using onpdf::test::testdata;

constexpr PageSize kLetter{612, 792};
constexpr PageSize kLetterPrintable{576, 756};  // 사방 18pt 여백

TEST(PrintLayout, PagesInRanges) {
  EXPECT_EQ(pages_in_ranges({{0, 4}}, 5), (std::vector<int>{0, 1, 2, 3, 4}));
  EXPECT_EQ(pages_in_ranges({{2, 2}, {0, 1}}, 5), (std::vector<int>{2, 0, 1}));
  EXPECT_EQ(pages_in_ranges({{3, 10}}, 5), (std::vector<int>{3, 4}));  // 범위 밖 자르기
  EXPECT_EQ(pages_in_ranges({{-3, 0}}, 5), (std::vector<int>{0}));
  EXPECT_EQ(pages_in_ranges({{4, 1}}, 5), (std::vector<int>{}));                    // 뒤집힌 구간
  EXPECT_EQ(pages_in_ranges({{0, 1}, {1, 2}}, 5), (std::vector<int>{0, 1, 1, 2}));  // 겹침 유지
  EXPECT_TRUE(pages_in_ranges({{7, 9}}, 5).empty());
  EXPECT_TRUE(pages_in_ranges({{0, 1}}, 0).empty());
  EXPECT_TRUE(pages_in_ranges({}, 5).empty());
}

TEST(PrintLayout, PrintSequenceCollates) {
  const auto collated = print_sequence({0, 1}, 3, true);
  ASSERT_EQ(collated.size(), 6u);
  EXPECT_EQ(collated[0].page, 0);
  EXPECT_EQ(collated[1].page, 1);
  EXPECT_EQ(collated[2].page, 0);
  for (const auto& step : collated) EXPECT_EQ(step.repeat, 1);

  const auto grouped = print_sequence({0, 1}, 3, false);
  ASSERT_EQ(grouped.size(), 2u);
  EXPECT_EQ(grouped[0].repeat, 3);
  EXPECT_EQ(grouped[1].page, 1);

  EXPECT_EQ(print_sequence({4}, 0, true).size(), 1u);  // 매수 하한 1
  EXPECT_TRUE(print_sequence({}, 2, true).empty());
}

TEST(PrintLayout, FitCentersPortraitOnPortrait) {
  auto p = place_page(kLetter, kLetterPrintable, PrintScaling::kFit);
  ASSERT_TRUE(p);
  EXPECT_EQ(p->rotation, 0);
  // 폭이 먼저 닿는다: 576/612 < 756/792
  EXPECT_NEAR(p->scale, 576.0 / 612.0, 1e-9);
  EXPECT_NEAR(p->target.w, 576.0, 1e-9);
  EXPECT_NEAR(p->target.x, 0.0, 1e-9);
  EXPECT_NEAR(p->target.y, (756.0 - p->target.h) / 2.0, 1e-9);
}

TEST(PrintLayout, LandscapePageRotatesOnPortraitPaper) {
  auto p = place_page({792, 612}, kLetterPrintable, PrintScaling::kFit);
  ASSERT_TRUE(p);
  EXPECT_EQ(p->rotation, 270);
  EXPECT_GT(p->target.h, p->target.w);  // 회전 후 세로로 긴 배치
  EXPECT_NEAR(p->scale, 576.0 / 612.0, 1e-9);

  auto no_rotate = place_page({792, 612}, kLetterPrintable, PrintScaling::kFit, false);
  ASSERT_TRUE(no_rotate);
  EXPECT_EQ(no_rotate->rotation, 0);
  EXPECT_NEAR(no_rotate->scale, 576.0 / 792.0, 1e-9);
  EXPECT_GT(no_rotate->target.w, no_rotate->target.h);

  // 정사각형 쪽은 회전하지 않는다
  auto square = place_page({500, 500}, kLetterPrintable, PrintScaling::kFit);
  ASSERT_TRUE(square);
  EXPECT_EQ(square->rotation, 0);
}

TEST(PrintLayout, ScalingModesForSmallAndLargePages) {
  const PageSize card{252, 144};  // 명함 (가로)
  auto fit = place_page(card, kLetterPrintable, PrintScaling::kFit);
  auto shrink = place_page(card, kLetterPrintable, PrintScaling::kShrinkOversized);
  auto actual = place_page(card, kLetterPrintable, PrintScaling::kActualSize);
  ASSERT_TRUE(fit && shrink && actual);
  EXPECT_GT(fit->scale, 1.0);            // 맞춤은 확대
  EXPECT_DOUBLE_EQ(shrink->scale, 1.0);  // 작은 쪽은 그대로
  EXPECT_DOUBLE_EQ(actual->scale, 1.0);

  const PageSize poster{1684, 2384};  // A1
  auto big_shrink = place_page(poster, kLetterPrintable, PrintScaling::kShrinkOversized);
  auto big_actual = place_page(poster, kLetterPrintable, PrintScaling::kActualSize);
  ASSERT_TRUE(big_shrink && big_actual);
  EXPECT_LT(big_shrink->scale, 1.0);
  EXPECT_LE(big_shrink->target.w, 576.0 + 1e-9);
  // 실제 크기: 가운데 기준으로 영역을 넘어 잘린다
  EXPECT_LT(big_actual->target.x, 0.0);
  EXPECT_LT(big_actual->target.y, 0.0);
  EXPECT_NEAR(big_actual->target.x + big_actual->target.w / 2.0, 288.0, 1e-9);
}

TEST(PrintLayout, RejectsInvalidSizes) {
  EXPECT_FALSE(place_page({0, 792}, kLetterPrintable, PrintScaling::kFit));
  EXPECT_FALSE(place_page(kLetter, {576, -1}, PrintScaling::kFit));
  EXPECT_FALSE(place_page({std::numeric_limits<double>::infinity(), 1}, kLetterPrintable,
                          PrintScaling::kFit));
  EXPECT_FALSE(place_page({std::numeric_limits<double>::quiet_NaN(), 1}, kLetterPrintable,
                          PrintScaling::kFit));
}

TEST(PrintLayout, ChoosePrintDpi) {
  const Rect letter{0, 0, 576, 756};
  EXPECT_EQ(choose_print_dpi(1200, true, letter), 300);
  EXPECT_EQ(choose_print_dpi(150, true, letter), 150);
  EXPECT_EQ(choose_print_dpi(0, true, letter), 300);      // 알 수 없는 해상도
  EXPECT_EQ(choose_print_dpi(1200, false, letter), 150);  // 저해상도 권한
  // A0 용지 전체(2384×3370pt): 300 DPI 면 1억 px 이상 → 상한에 맞춰 하향
  const Rect a0{0, 0, 2384, 3370};
  const int dpi = choose_print_dpi(1200, true, a0);
  EXPECT_LT(dpi, 300);
  const double px = (2384.0 / 72.0) * (3370.0 / 72.0) * dpi * dpi;
  EXPECT_LE(px, static_cast<double>(kMaxPrintPixels));
  // 아주 큰 영역도 하한 72
  EXPECT_EQ(choose_print_dpi(1200, true, Rect{0, 0, 72 * 1000, 72 * 1000}), kMinPrintDpi);
}

// ---------------------------------------------------------------- PDFium

std::uint32_t pixel_at(const Bitmap& bmp, int x, int y) {
  const auto* p = bmp.pixels.data() +
                  static_cast<std::size_t>(y) * static_cast<std::size_t>(bmp.stride) +
                  static_cast<std::size_t>(x) * 4;
  return (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[1]) << 8) |
         p[0];  // RGB
}
bool is_greenish(std::uint32_t rgb) {
  return ((rgb >> 8) & 0xFF) > 120 && ((rgb >> 16) & 0xFF) < 60;
}
bool is_reddish(std::uint32_t rgb) {
  return ((rgb >> 16) & 0xFF) > 180 && ((rgb >> 8) & 0xFF) < 60;
}
bool is_white(std::uint32_t rgb) {
  return rgb == 0xFFFFFF;
}

TEST(PrintRender, PrintFlagSelectsAnnotations) {
  auto doc = PdfiumDocument::open(testdata("print-flags.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  engine::RenderOptions screen;
  auto s = doc->render_page(0, screen);
  ASSERT_TRUE(s) << s.error().message;
  engine::RenderOptions print;
  print.printing = true;
  auto pr = doc->render_page(0, print);
  ASSERT_TRUE(pr) << pr.error().message;
  // 상자 중심 (사용자 좌표 → 좌상단 원점 픽셀, 배율 1)
  const int y = 792 - 580;
  const int green_x = 172;
  const int red_x = 440;
  EXPECT_TRUE(is_greenish(pixel_at(*s, green_x, y))) << std::hex << pixel_at(*s, green_x, y);
  EXPECT_TRUE(is_white(pixel_at(*s, red_x, y))) << std::hex << pixel_at(*s, red_x, y);
  EXPECT_TRUE(is_white(pixel_at(*pr, green_x, y))) << std::hex << pixel_at(*pr, green_x, y);
  EXPECT_TRUE(is_reddish(pixel_at(*pr, red_x, y))) << std::hex << pixel_at(*pr, red_x, y);
}

TEST(PrintRender, GrayscaleFlag) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  engine::RenderOptions gray;
  gray.grayscale = true;
  auto bmp = doc->render_page(0, gray);
  ASSERT_TRUE(bmp) << bmp.error().message;
  // 파란 사각형 안쪽: 회색조면 R=G=B 이고 흰색이 아니다
  auto color = doc->render_page(0, {});
  ASSERT_TRUE(color);
  int checked = 0;
  for (int y = 0; y < bmp->height; y += 7) {
    for (int x = 0; x < bmp->width; x += 7) {
      const std::uint32_t c = pixel_at(*color, x, y);
      if ((c & 0xFF) > 200 && ((c >> 16) & 0xFF) < 40) {  // 원본에서 파란 픽셀
        const std::uint32_t g = pixel_at(*bmp, x, y);
        EXPECT_EQ((g >> 16) & 0xFF, g & 0xFF);
        EXPECT_EQ((g >> 8) & 0xFF, g & 0xFF);
        EXPECT_FALSE(is_white(g));
        ++checked;
      }
    }
  }
  EXPECT_GT(checked, 10);
}

TEST(PrintPreferencesLoad, ViewerPreferences) {
  auto doc = PdfiumDocument::open(testdata("print-flags.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  auto prefs = doc->print_preferences();
  ASSERT_TRUE(prefs) << prefs.error().message;
  EXPECT_TRUE(prefs->allowed);
  EXPECT_TRUE(prefs->high_quality);
  EXPECT_TRUE(prefs->no_scaling);
  EXPECT_EQ(prefs->copies, 2);
  ASSERT_EQ(prefs->page_ranges.size(), 1u);
  EXPECT_EQ(prefs->page_ranges[0],
            (std::pair<int, int>{1, 1}));  // /PrintPageRange [2 2] → 0 기준 2쪽
  EXPECT_EQ(prefs->duplex, Duplex::kFlipLongEdge);
}

TEST(PrintPreferencesLoad, DefaultsWithoutPreferences) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  auto prefs = doc->print_preferences();
  ASSERT_TRUE(prefs);
  EXPECT_TRUE(prefs->allowed);
  EXPECT_TRUE(prefs->high_quality);
  EXPECT_FALSE(prefs->no_scaling);
  EXPECT_EQ(prefs->copies, 1);
  EXPECT_TRUE(prefs->page_ranges.empty());
  EXPECT_EQ(prefs->duplex, Duplex::kUndefined);
}

TEST(PrintPreferencesLoad, Permissions) {
  auto denied = PdfiumDocument::open(testdata("encrypted-owner-only.pdf"));
  ASSERT_TRUE(denied) << denied.error().message;
  EXPECT_FALSE(denied->print_preferences()->allowed);
  EXPECT_FALSE(denied->print_preferences()->high_quality);

  // 소유자 암호로 열면 제한이 풀린다
  auto owner = PdfiumDocument::open(testdata("encrypted-owner-only.pdf"), "owner");
  ASSERT_TRUE(owner) << owner.error().message;
  EXPECT_TRUE(owner->print_preferences()->allowed);

  auto low = PdfiumDocument::open(testdata("print-low-res.pdf"));
  ASSERT_TRUE(low) << low.error().message;
  EXPECT_TRUE(low->print_preferences()->allowed);
  EXPECT_FALSE(low->print_preferences()->high_quality);

  auto user = PdfiumDocument::open(testdata("encrypted-rc4-user.pdf"), "user");
  ASSERT_TRUE(user) << user.error().message;
  EXPECT_TRUE(user->print_preferences()->allowed);
  EXPECT_TRUE(user->print_preferences()->high_quality);
}

}  // namespace
}  // namespace onpdf::view
