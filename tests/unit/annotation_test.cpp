// PdfiumDocument 주석 API: 좌표 변환·조회·생성·AP 규칙·스냅샷 복원 (#35)
#include "onpdf/core/engine/pdfium_document.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

namespace onpdf::engine {
namespace {

using onpdf::test::testdata;

const AnnotationInfo* find_id(const std::vector<AnnotationInfo>& list, const std::string& id) {
  for (const auto& a : list) if (a.id == id) return &a;
  return nullptr;
}

// 사각형 안에서 조건을 만족하는 픽셀 비율 (BGRA)
double fraction_in(const Bitmap& bmp, const Rect& rect_pt, double scale, bool (*pred)(int r, int g, int b)) {
  const int x0 = std::max(0, static_cast<int>(rect_pt.x * scale)), y0 = std::max(0, static_cast<int>(rect_pt.y * scale));
  const int x1 = std::min(bmp.width, static_cast<int>(rect_pt.right() * scale));
  const int y1 = std::min(bmp.height, static_cast<int>(rect_pt.bottom() * scale));
  int hit = 0, total = 0;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const auto* p = bmp.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(bmp.stride) +
                      static_cast<std::size_t>(x) * 4;
      ++total;
      if (pred(p[2], p[1], p[0])) ++hit;
    }
  }
  return total == 0 ? 0.0 : static_cast<double>(hit) / total;
}
bool yellowish(int r, int g, int b) { return r > 200 && g > 200 && b < 80; }
bool greenish(int r, int g, int b) { return g > 200 && r < 80 && b < 80; }

// 메모 아이콘 사각형이 회전된 쪽에서도 입력한 쪽 포인트 자리로 돌아온다 (to_user ↔ to_rect 왕복)
TEST(Annotations, RectRoundTripsOnRotatedPages) {
  for (int rotation = 0; rotation < 4; ++rotation) {
    auto doc = PdfiumDocument::open(testdata("simple.pdf"));
    ASSERT_TRUE(doc);
    ASSERT_TRUE(doc->set_page_rotation(0, rotation));
    AnnotationSpec spec;
    spec.kind = AnnotationKind::kText;
    spec.rect = {100, 150, 20, 20};
    spec.flags = kAnnotFlagPrint | kAnnotFlagNoZoom | kAnnotFlagNoRotate;
    spec.contents = "메모 한글";
    auto id = doc->add_annotation(0, spec);
    ASSERT_TRUE(id) << id.error().message;
    auto list = doc->annotations(0);
    ASSERT_TRUE(list);
    const AnnotationInfo* a = find_id(*list, *id);
    ASSERT_NE(a, nullptr) << "rotation " << rotation;
    EXPECT_NEAR(a->rect.x, 100, 0.05) << rotation;
    EXPECT_NEAR(a->rect.y, 150, 0.05) << rotation;
    EXPECT_NEAR(a->rect.w, 20, 0.05) << rotation;
    EXPECT_NEAR(a->rect.h, 20, 0.05) << rotation;
    EXPECT_EQ(a->kind, AnnotationKind::kText);
    EXPECT_EQ(a->contents, "메모 한글");
    EXPECT_TRUE(a->editable);
    EXPECT_EQ(a->flags & kAnnotFlagPrint, kAnnotFlagPrint);
    EXPECT_TRUE(a->has_color);  // 아직 AP 없음
  }
}

// 선택 사각형(쪽 포인트)으로 만든 하이라이트의 Rect 는 사각형들의 합집합이고, 색 변경은 AP 가 생긴 뒤에도 된다
TEST(Annotations, HighlightFromSelectionAndRecolorAfterRender) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  auto text = doc->load_text(0);
  ASSERT_TRUE(text);
  const std::vector<Rect> quads = text->selection_rects({0, std::min(10, text->char_count())});
  ASSERT_FALSE(quads.empty());
  AnnotationSpec spec;
  spec.kind = AnnotationKind::kHighlight;
  spec.quads = quads;
  spec.color = {255, 255, 0, 255};
  auto id = doc->add_annotation(0, spec);
  ASSERT_TRUE(id) << id.error().message;
  auto list = doc->annotations(0);
  ASSERT_TRUE(list);
  const AnnotationInfo* a = find_id(*list, *id);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->kind, AnnotationKind::kHighlight);
  EXPECT_NEAR(a->rect.x, quads.front().x, 0.05);
  EXPECT_NEAR(a->rect.y, quads.front().y, 0.05);
  // 렌더 → 노란 하이라이트, AP 생성
  RenderOptions options;
  options.scale = 2.0f;
  auto before = doc->render_page(0, options);
  ASSERT_TRUE(before);
  EXPECT_GT(fraction_in(*before, quads.front(), 2.0, yellowish), 0.3);
  auto snap = doc->snapshot_annotation(0, *id);
  ASSERT_TRUE(snap);
  EXPECT_FALSE(snap->ap_normal.empty()) << "PDFium generates an AP on render";
  EXPECT_EQ(snap->quads.size(), quads.size());
  // AP 가 있어도 색 변경이 되고 다음 렌더가 초록으로 그린다
  ASSERT_TRUE(doc->set_annotation_color(0, *id, {0, 255, 0, 255}, {}));
  auto after = doc->render_page(0, options);
  ASSERT_TRUE(after);
  EXPECT_GT(fraction_in(*after, quads.front(), 2.0, greenish), 0.3);
  EXPECT_LT(fraction_in(*after, quads.front(), 2.0, yellowish), 0.05);
  // 이전 AP 로 되돌리면 다시 노랑
  ASSERT_TRUE(doc->set_annotation_color(0, *id, {0, 0, 0, 255}, snap->ap_normal));
  auto restored = doc->render_page(0, options);
  ASSERT_TRUE(restored);
  EXPECT_GT(fraction_in(*restored, quads.front(), 2.0, yellowish), 0.3);
}

TEST(Annotations, ExistingAnnotationsAreListedWithKindsAndEditability) {
  auto doc = PdfiumDocument::open(testdata("annotations.pdf"));
  ASSERT_TRUE(doc);
  auto list = doc->annotations(0);
  ASSERT_TRUE(list);
  ASSERT_EQ(list->size(), 6u);
  EXPECT_EQ((*list)[0].kind, AnnotationKind::kHighlight);
  EXPECT_FALSE((*list)[0].has_color) << "AP 가 있는 주석은 색을 읽지 못한다";
  EXPECT_FALSE((*list)[0].contents.empty());
  EXPECT_EQ((*list)[0].id, "#0");  // NM 없음
  EXPECT_EQ((*list)[1].kind, AnnotationKind::kSquare);
  EXPECT_TRUE((*list)[1].has_color);
  EXPECT_EQ((*list)[2].kind, AnnotationKind::kText);
  EXPECT_EQ((*list)[2].author, "작성자");
  for (int i = 3; i < 6; ++i) {
    EXPECT_EQ((*list)[static_cast<std::size_t>(i)].kind, AnnotationKind::kLink);
    EXPECT_FALSE((*list)[static_cast<std::size_t>(i)].editable);
  }
  // 스냅샷은 NM 을 부여한다
  auto snap = doc->snapshot_annotation(0, "#1");
  ASSERT_TRUE(snap);
  EXPECT_EQ(snap->id.rfind("onpdf-", 0), 0u) << snap->id;  // 부여된 NM 접두
  auto again = doc->annotations(0);
  EXPECT_EQ((*again)[1].id, snap->id);
}

// 기존(AP 있는) 하이라이트를 지우고 되살리면 모양·내용이 그대로다
TEST(Annotations, RemoveAndRestoreKeepsAppearance) {
  auto doc = PdfiumDocument::open(testdata("annotations.pdf"));
  ASSERT_TRUE(doc);
  auto snap = doc->snapshot_annotation(0, "#0");
  ASSERT_TRUE(snap);
  ASSERT_FALSE(snap->ap_normal.empty());
  ASSERT_EQ(snap->quads.size(), 1u);
  ASSERT_TRUE(doc->remove_annotation(0, snap->id));
  EXPECT_EQ(doc->annotations(0)->size(), 5u);
  ASSERT_TRUE(doc->restore_annotation(0, *snap));
  auto list = doc->annotations(0);
  ASSERT_EQ(list->size(), 6u);
  const AnnotationInfo* back = find_id(*list, snap->id);
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(back->kind, AnnotationKind::kHighlight);
  EXPECT_EQ(back->contents, snap->contents);
  auto snap2 = doc->snapshot_annotation(0, snap->id);
  ASSERT_TRUE(snap2);
  EXPECT_EQ(snap2->ap_normal, snap->ap_normal);
  EXPECT_EQ(snap2->quads, snap->quads);
  EXPECT_EQ(snap2->flags, snap->flags);
  // 없는 id
  EXPECT_FALSE(doc->remove_annotation(0, "nope"));
  EXPECT_FALSE(doc->snapshot_annotation(0, "#99"));
}

TEST(Annotations, InkAndContents) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  AnnotationSpec ink;
  ink.kind = AnnotationKind::kInk;
  ink.ink = {{{100, 400}, {150, 380}, {200, 400}}};
  ink.rect = {98, 378, 104, 24};
  ink.border_width = 2.0;
  ink.color = {255, 0, 0, 255};
  auto id = doc->add_annotation(0, ink);
  ASSERT_TRUE(id) << id.error().message;
  auto snap = doc->snapshot_annotation(0, *id);
  ASSERT_TRUE(snap);
  ASSERT_EQ(snap->ink.size(), 1u);
  EXPECT_EQ(snap->ink[0].size(), 3u);
  EXPECT_TRUE(snap->has_border);
  EXPECT_FLOAT_EQ(snap->border_width, 2.0f);
  ASSERT_TRUE(doc->set_annotation_contents(0, *id, "펜 메모"));
  EXPECT_EQ(find_id(*doc->annotations(0), *id)->contents, "펜 메모");
  AnnotationSpec bad;
  bad.kind = AnnotationKind::kHighlight;  // quads 없음
  EXPECT_FALSE(doc->add_annotation(0, bad));
  bad.kind = AnnotationKind::kLink;
  EXPECT_FALSE(doc->add_annotation(0, bad));
}

}  // namespace
}  // namespace onpdf::engine
