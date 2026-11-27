#pragma once

// 왕복 검사 공용 헬퍼 — manifest 왕복(roundtrip_test.cpp)과 코퍼스 왕복(corpus_test.cpp, #119)이
// 같은 검사를 돈다: 열기 → 안전 저장(서명 문서면 증분) → 다시 열기 → 쪽 수·쪽 크기·첫 쪽 텍스트·
// 첫 쪽 렌더 비교 → qpdf 라이브러리 구조 검사. 실패 메시지는 "<label> stage=<단계>: <사유>".

#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/qpdf_tools.h"
#include "onpdf/core/engine/save.h"

#include "../test_support.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <iostream>
#include <string>

namespace onpdf::test {

// 렌더 결과 비교: 채널 차이가 허용치를 넘는 픽셀 비율
inline double different_fraction(const Bitmap& a, const Bitmap& b) {
  if (a.width != b.width || a.height != b.height) return 1.0;
  std::size_t diff = 0;
  for (int y = 0; y < a.height; ++y) {
    for (int x = 0; x < a.width; ++x) {
      const auto* pa = a.pixels.data() +
                       static_cast<std::size_t>(y) * static_cast<std::size_t>(a.stride) +
                       static_cast<std::size_t>(x) * 4;
      const auto* pb = b.pixels.data() +
                       static_cast<std::size_t>(y) * static_cast<std::size_t>(b.stride) +
                       static_cast<std::size_t>(x) * 4;
      for (int c = 0; c < 3; ++c) {
        if (std::abs(int{pa[c]} - int{pb[c]}) > kChannelTolerance) {
          ++diff;
          break;
        }
      }
    }
  }
  return static_cast<double>(diff) / static_cast<double>(std::max(1, a.width * a.height));
}

// 첫 쪽 비교 렌더 배율: 기본 0.5 이되 픽셀 수가 상한(4M px)을 넘는 거대 쪽은 낮춘다
inline float bounded_render_scale(const engine::PageSize& size, float wanted = 0.5f) {
  constexpr double kMaxPixels = 4.0 * 1024 * 1024;
  const double area = static_cast<double>(size.width) * static_cast<double>(size.height);
  if (area <= 0.0) return wanted;
  const double limit = std::sqrt(kMaxPixels / area);
  return static_cast<float>(std::min<double>(wanted, limit));
}

// 왕복 검사 본문. gtest 의 ASSERT 는 void 함수에서만 쓸 수 있으므로 void 다 — 케이스마다 한 번
// 부른다.
inline void expect_roundtrip(const std::filesystem::path& file, const std::string& password,
                             const std::string& label) {
  auto original = engine::PdfiumDocument::open(file, password);
  ASSERT_TRUE(original) << label << " stage=open: " << original.error().message;

  ScopedTempDir dir;
  const auto target = dir / "roundtrip.pdf";
  engine::SaveOptions options;
  options.incremental = original->signature_count() > 0;
  auto saved = engine::save_document_safely(*original, target, options);
  ASSERT_TRUE(saved) << label << " stage=save: " << saved.error().message;

  auto reopened = engine::PdfiumDocument::open(target, password);
  ASSERT_TRUE(reopened) << label << " stage=reopen: " << reopened.error().message;
  ASSERT_EQ(reopened->page_count(), original->page_count()) << label << " stage=compare";
  for (int i = 0; i < original->page_count(); ++i) {
    auto before = original->page_size(i);
    auto after = reopened->page_size(i);
    ASSERT_TRUE(before && after) << label << " stage=compare: page " << i << " size";
    EXPECT_NEAR(after->width, before->width, 0.01) << label << " stage=compare: page " << i;
    EXPECT_NEAR(after->height, before->height, 0.01) << label << " stage=compare: page " << i;
  }
  if (original->page_count() > 0) {
    auto text_before = original->load_text(0);
    auto text_after = reopened->load_text(0);
    ASSERT_TRUE(text_before && text_after) << label << " stage=compare: first page text";
    EXPECT_EQ(text_after->char_count(), text_before->char_count()) << label << " stage=compare";
    EXPECT_EQ(text_after->all_text(), text_before->all_text()) << label << " stage=compare";
    auto size = original->page_size(0);
    ASSERT_TRUE(size) << label << " stage=compare: first page size";
    engine::RenderOptions render;
    render.scale = bounded_render_scale(*size);
    auto bitmap_before = original->render_page(0, render);
    auto bitmap_after = reopened->render_page(0, render);
    ASSERT_TRUE(bitmap_before && bitmap_after) << label << " stage=compare: first page render";
    EXPECT_LE(different_fraction(*bitmap_before, *bitmap_after), kMaxDifferentFraction)
        << label << " stage=compare: first page pixels";
  }

  // 라이브러리 qpdf 로 구조 검사 (CI 에도 항상 돈다). 경고(원본에도 있던 경고 포함)는 통과.
  // 원본부터 qpdf 가 오류로 거절하는 문서(PDFium 만 복구해 여는 손상 파일, qpdf 가 모르는 Brotli
  // 필터, qpdf 가 열지 못하는 암호 설정)는 저장본의 qpdf 오류를 우리 탓으로 보지 않는다 — PDFium
  // 다시 열기·비교까지는 위에서 통과해야 한다
  auto checked = engine::QpdfTools::check(target, password);
  if (!checked) {
    auto original_checked = engine::QpdfTools::check(file, password);
    if (!original_checked) {
      std::cout << "[  NOTE    ] " << label << ": qpdf rejects the original too ("
                << original_checked.error().message << ")\n";
      return;
    }
  }
  EXPECT_TRUE(checked) << label << " stage=qpdf: " << (checked ? "" : checked.error().message);
}

}  // namespace onpdf::test
