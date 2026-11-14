// 실제 PDF 에서 텍스트 인덱스 추출 (전역 PdfiumLibrary 환경)

#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/view/text_index.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace onpdf::engine {
namespace {

using onpdf::test::testdata;

// 글자 상자(포인트)가 렌더된 글리프 위에 놓이는지: 쪽을 scale 2 로 그려 각 상자 안에 어두운 픽셀이 있어야 한다.
// 다른 그래픽(테두리·막대)이 있어도 영향받지 않고, 좌표계가 어긋나면(회전 미반영 등) 대부분 실패한다.
void expect_char_boxes_contain_ink(const char* file, int page) {
  auto doc = PdfiumDocument::open(testdata(file));
  ASSERT_TRUE(doc) << doc.error().message;
  auto index = doc->load_text(page);
  ASSERT_TRUE(index) << index.error().message;
  ASSERT_GT(index->char_count(), 0);
  RenderOptions options;
  options.scale = 2.0f;
  auto bitmap = doc->render_page(page, options);
  ASSERT_TRUE(bitmap);
  int boxed = 0, inked = 0;
  for (const auto& c : index->chars) {
    if (c.box.empty() || c.code == U' ') continue;
    ++boxed;
    const int x0 = std::max(0, static_cast<int>(c.box.x * 2)), y0 = std::max(0, static_cast<int>(c.box.y * 2));
    const int x1 = std::min(bitmap->width, static_cast<int>(c.box.right() * 2) + 1);
    const int y1 = std::min(bitmap->height, static_cast<int>(c.box.bottom() * 2) + 1);
    bool dark = false;
    for (int y = y0; y < y1 && !dark; ++y) {
      const std::uint8_t* row = bitmap->row(y);
      for (int x = x0; x < x1; ++x) {
        if (row[x * 4] < 160 || row[x * 4 + 1] < 160 || row[x * 4 + 2] < 160) {
          dark = true;
          break;
        }
      }
    }
    if (dark) ++inked;
  }
  ASSERT_GT(boxed, 0);
  EXPECT_GE(inked, boxed * 9 / 10) << file << " page " << page << ": " << inked << " / " << boxed
                                   << " char boxes contain ink";
}

TEST(TextPage, KoreanEmbeddedTextAndLines) {
  auto doc = PdfiumDocument::open(testdata("korean-embedded.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  auto index = doc->load_text(0);
  ASSERT_TRUE(index) << index.error().message;
  EXPECT_EQ(index->char_count(), 103);
  EXPECT_EQ(index->lines.size(), 4u);
  const std::string text = index->all_text();
  EXPECT_NE(text.find("안녕하세요, OnPdf 입니다."), std::string::npos);
  EXPECT_EQ(index->text(index->lines[0].chars), "안녕하세요, OnPdf 입니다.");
  EXPECT_TRUE(index->links.empty());
  // 줄은 위에서 아래로
  for (std::size_t i = 1; i < index->lines.size(); ++i) {
    EXPECT_GT(index->lines[i].box.y, index->lines[i - 1].box.y);
  }
}

TEST(TextPage, SystemFontKoreanViaCMap) {
  auto doc = PdfiumDocument::open(testdata("korean-system-font.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  auto index = doc->load_text(0);
  ASSERT_TRUE(index) << index.error().message;
  EXPECT_NE(index->all_text().find("시스템 폰트"), std::string::npos);
}

TEST(TextPage, ScannedPageHasNoText) {
  auto doc = PdfiumDocument::open(testdata("scanned.pdf"));
  ASSERT_TRUE(doc);
  auto index = doc->load_text(0);
  ASSERT_TRUE(index) << index.error().message;
  EXPECT_EQ(index->char_count(), 0);
  EXPECT_TRUE(index->lines.empty());
  EXPECT_EQ(index->nearest_position({10, 10}), 0);
}

TEST(TextPage, CharBoxesMatchRenderedContent) {
  expect_char_boxes_contain_ink("korean-embedded.pdf", 0);
  expect_char_boxes_contain_ink("simple.pdf", 0);
}

TEST(TextPage, RotatedPageBoxesMatchRenderAndFormOneLine) {
  expect_char_boxes_contain_ink("mixed-sizes.pdf", 5);
  expect_char_boxes_contain_ink("mixed-sizes.pdf", 6);
  auto doc = PdfiumDocument::open(testdata("mixed-sizes.pdf"));
  ASSERT_TRUE(doc);
  auto index = doc->load_text(5);
  ASSERT_TRUE(index);
  EXPECT_EQ(index->lines.size(), 1u);
  EXPECT_NE(index->all_text().find("Rotate 90"), std::string::npos);
  // 회전된 쪽에서는 글자가 세로로 쌓인다(쪽 포인트 y 증가, x 거의 같음) — 좌표계가 렌더와 같다는 증거
  const auto& a = index->chars[0].box;
  const auto& b = index->chars[1].box;
  EXPECT_NEAR(a.x, b.x, a.w + 1.0);
  EXPECT_GT(std::abs(b.y - a.y), 1.0);
}

TEST(TextPage, LinksFromAnnotationsPdf) {
  auto doc = PdfiumDocument::open(testdata("annotations.pdf"));
  ASSERT_TRUE(doc);
  auto index = doc->load_text(0);
  ASSERT_TRUE(index) << index.error().message;
  ASSERT_EQ(index->links.size(), 3u);
  int uri = 0, page = 0, blocked = 0;
  for (const auto& link : index->links) {
    if (link.kind == view::LinkKind::kUri) {
      ++uri;
      if (!view::is_safe_uri(link.uri)) ++blocked;
    } else {
      ++page;
      EXPECT_EQ(link.page, 1);
      ASSERT_TRUE(link.location.has_value());
      // /XYZ 72 700 → 쪽 포인트 (72, 792-700=92)
      EXPECT_NEAR(link.location->x, 72.0, 0.01);
      EXPECT_NEAR(link.location->y, 92.0, 0.01);
    }
    EXPECT_FALSE(link.box.empty());
  }
  EXPECT_EQ(uri, 2);
  EXPECT_EQ(page, 1);
  EXPECT_EQ(blocked, 1);
  // 링크 사각형 hit: 내부 링크 Rect [72 360 300 380] → 쪽 포인트 y = 792-380 .. 792-360
  const auto* hit = index->link_at({100, 792 - 370});
  ASSERT_NE(hit, nullptr);
  EXPECT_EQ(hit->kind, view::LinkKind::kPage);
}

// 줄 묶음 휴리스틱: 회전 텍스트 매트릭스·1 Tf+Tm 스케일·위첨자·2단 (리뷰 W1/W2 회귀)
TEST(TextPage, LineHeuristicsHandleRotatedMatrixAndScaledFont) {
  auto doc = PdfiumDocument::open(testdata("text-layout.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  auto index = doc->load_text(0);
  ASSERT_TRUE(index) << index.error().message;
  std::vector<std::string> lines;
  for (const auto& line : index->lines) lines.push_back(index->text(line.chars));
  auto has = [&](const char* needle) {
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return l == needle; });
  };
  EXPECT_TRUE(has("Normal line of text")) << index->all_text();
  EXPECT_TRUE(has("Rotated text matrix")) << index->all_text();   // 회전 Tm 이 한 줄
  EXPECT_TRUE(has("E=mc2")) << index->all_text();                 // 1 Tf 스케일 + 위첨자가 한 줄
  // 같은 기준선의 두 단은 PDFium 이 읽기 순서로 합치거나 나눌 수 있다 — 어느 쪽이든 한 줄에 두 단어가 모두 있거나 각각 한 줄
  const bool columns_ok = has("Left column Right column") || has("Right column Left column") ||
                          (has("Left column") && has("Right column"));
  EXPECT_TRUE(columns_ok) << index->all_text();
  EXPECT_LE(index->lines.size(), 5u) << index->all_text();
  // 회전 줄은 세로로 길고, 그 줄에서 nearest_position 이 y 축으로 동작한다
  const auto rotated = std::find_if(index->lines.begin(), index->lines.end(),
                                    [&](const view::TextLine& l) { return index->text(l.chars) == "Rotated text matrix"; });
  ASSERT_NE(rotated, index->lines.end());
  EXPECT_GT(rotated->box.h, rotated->box.w);
  const int first = rotated->chars.start;
  const auto& c0 = index->chars[static_cast<std::size_t>(first)].box;
  const auto& c1 = index->chars[static_cast<std::size_t>(first + 1)].box;
  EXPECT_LT(c1.y, c0.y);  // 위로 흐르는 글: 둘째 글자가 더 위
  // 첫 글자 중심에서 읽기 방향으로 살짝 지난 지점(둘째 글자 쪽) → 첫 글자 뒤 = first+1
  const double c0_center = c0.y + c0.h / 2, c1_center = c1.y + c1.h / 2;
  const int just_after_first = index->nearest_position({c0.x + c0.w / 2, c0_center - (c0_center - c1_center) * 0.3});
  EXPECT_EQ(just_after_first, first + 1);
  // 줄의 위쪽 끝(읽기 방향 끝) 너머 → 줄 끝, 아래쪽 너머 → 줄 시작
  EXPECT_EQ(index->nearest_position({c0.x + c0.w / 2, rotated->box.y - 5}), rotated->chars.end);
  EXPECT_EQ(index->nearest_position({c0.x + c0.w / 2, rotated->box.bottom() + 5}), rotated->chars.start);
  // source_index 는 PDFium 인덱스 (줄바꿈 문자를 버리므로 단조 증가하되 건너뛸 수 있음)
  for (std::size_t i = 1; i < index->chars.size(); ++i) {
    EXPECT_GT(index->chars[i].source_index, index->chars[i - 1].source_index);
  }
}

TEST(TextPage, BookmarksPdfHasNoLinkAnnotations) {
  auto doc = PdfiumDocument::open(testdata("bookmarks.pdf"));
  ASSERT_TRUE(doc);
  auto index = doc->load_text(0);
  ASSERT_TRUE(index);
  EXPECT_TRUE(index->links.empty());
  EXPECT_GT(index->char_count(), 0);
}

}  // namespace
}  // namespace onpdf::engine
