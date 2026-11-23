// PdfiumDocument 주석 API: 좌표 변환·조회·생성·AP 규칙·스냅샷 복원 (#35)
#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/qpdf_tools.h"
#include "onpdf/core/engine/save.h"

#include "../test_support.h"
#include <cmath>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace onpdf::engine {
namespace {

using onpdf::test::ScopedTempDir;
using onpdf::test::testdata;

const AnnotationInfo* find_id(const std::vector<AnnotationInfo>& list, const std::string& id) {
  for (const auto& a : list)
    if (a.id == id) return &a;
  return nullptr;
}

// 사각형 안에서 조건을 만족하는 픽셀 비율 (BGRA)
double fraction_in(const Bitmap& bmp, const Rect& rect_pt, double scale,
                   bool (*pred)(int r, int g, int b)) {
  const int x0 = std::max(0, static_cast<int>(rect_pt.x * scale)),
            y0 = std::max(0, static_cast<int>(rect_pt.y * scale));
  const int x1 = std::min(bmp.width, static_cast<int>(rect_pt.right() * scale));
  const int y1 = std::min(bmp.height, static_cast<int>(rect_pt.bottom() * scale));
  int hit = 0, total = 0;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const auto* p = bmp.pixels.data() +
                      static_cast<std::size_t>(y) * static_cast<std::size_t>(bmp.stride) +
                      static_cast<std::size_t>(x) * 4;
      ++total;
      if (pred(p[2], p[1], p[0])) ++hit;
    }
  }
  return total == 0 ? 0.0 : static_cast<double>(hit) / total;
}
bool yellowish(int r, int g, int b) {
  return r > 200 && g > 200 && b < 80;
}
bool greenish(int r, int g, int b) {
  return g > 200 && r < 80 && b < 80;
}
bool dark(int r, int g, int b) {
  return r < 90 && g < 90 && b < 90;
}  // 글자

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

// 선택 사각형(쪽 포인트)으로 만든 하이라이트의 Rect 는 사각형들의 합집합이고, 색 변경은 AP 가 생긴
// 뒤에도 된다
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
  const double text_before = fraction_in(*before, quads.front(), 2.0, dark);
  ASSERT_GT(text_before, 0.02) << "글자가 하이라이트를 통해 보여야 한다 (Multiply)";
  auto snap = doc->snapshot_annotation(0, *id);  // AP 를 지우고 색을 읽는다
  ASSERT_TRUE(snap);
  EXPECT_TRUE(snap->has_color);
  EXPECT_EQ(snap->color, (Color{255, 255, 0, 255}));
  EXPECT_EQ(snap->quads.size(), quads.size());
  // AP 가 있어도 색 변경이 되고 다음 렌더가 초록으로 그린다
  ASSERT_TRUE(doc->set_annotation_color(0, *id, {0, 255, 0, 255}));
  auto after = doc->render_page(0, options);
  ASSERT_TRUE(after);
  EXPECT_GT(fraction_in(*after, quads.front(), 2.0, greenish), 0.3);
  EXPECT_LT(fraction_in(*after, quads.front(), 2.0, yellowish), 0.05);
  // 이전 색으로 되돌리면 다시 노랑이고 글자도 그대로 보인다 (리뷰 C1: AP 텍스트 복원은 /Resources
  // 를 잃어 글자를 덮었다)
  ASSERT_TRUE(doc->set_annotation_color(0, *id, snap->color));
  auto restored = doc->render_page(0, options);
  ASSERT_TRUE(restored);
  EXPECT_GT(fraction_in(*restored, quads.front(), 2.0, yellowish), 0.3);
  EXPECT_NEAR(fraction_in(*restored, quads.front(), 2.0, dark), text_before, 0.01);
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

// 기존(AP 있는) 하이라이트를 지우고 되살리면 색·Quad·내용이 그대로고 글자가 여전히 보인다 (리뷰 C1)
TEST(Annotations, RemoveAndRestoreKeepsTextVisible) {
  auto doc = PdfiumDocument::open(testdata("annotations.pdf"));
  ASSERT_TRUE(doc);
  RenderOptions options;
  options.scale = 2.0f;
  const Rect hl = (*doc->annotations(0))[0].rect;
  auto before = doc->render_page(0, options);
  ASSERT_TRUE(before);
  const double text_before = fraction_in(*before, hl, 2.0, dark);
  ASSERT_GT(text_before, 0.02);
  auto snap = doc->snapshot_annotation(0, "#0");
  ASSERT_TRUE(snap);
  EXPECT_TRUE(snap->has_color);
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
  auto after = doc->render_page(0, options);
  ASSERT_TRUE(after);
  EXPECT_GT(fraction_in(*after, hl, 2.0, yellowish), 0.3);
  EXPECT_NEAR(fraction_in(*after, hl, 2.0, dark), text_before, 0.01)
      << "복원된 하이라이트가 글자를 덮으면 안 된다";
  auto snap2 = doc->snapshot_annotation(0, snap->id);
  ASSERT_TRUE(snap2);
  EXPECT_EQ(snap2->color, snap->color);
  EXPECT_EQ(snap2->quads, snap->quads);
  EXPECT_EQ(snap2->flags, snap->flags);
  // Square: 내부색(/IC)과 불투명도(/CA) 도 왕복한다 (리뷰 W3). 복원된 하이라이트가 끝으로 갔으니
  // Square 는 이제 #0
  auto sq = doc->snapshot_annotation(0, "#0");
  ASSERT_TRUE(sq);
  EXPECT_EQ(sq->kind, AnnotationKind::kSquare);
  EXPECT_TRUE(sq->has_interior);
  EXPECT_EQ(sq->interior.r, 255);
  EXPECT_EQ(sq->color.a, 204);  // CA 0.8
  ASSERT_TRUE(doc->remove_annotation(0, sq->id));
  ASSERT_TRUE(doc->restore_annotation(0, *sq));
  auto sq2 = doc->snapshot_annotation(0, sq->id);
  ASSERT_TRUE(sq2);
  EXPECT_EQ(sq2->interior, sq->interior);
  EXPECT_EQ(sq2->color, sq->color);
  // 없는 id
  EXPECT_FALSE(doc->remove_annotation(0, "nope"));
  EXPECT_FALSE(doc->snapshot_annotation(0, "#99"));
}

// 외부 메모의 /Popup 은 메모와 함께 지워지고, 복원은 메모만 되살린다 (리뷰 I8)
TEST(Annotations, RemovingNoteRemovesLinkedPopup) {
  auto doc = PdfiumDocument::open(testdata("annotation-popup.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  auto list = doc->annotations(0);
  ASSERT_TRUE(list);
  ASSERT_EQ(list->size(), 2u);
  EXPECT_EQ((*list)[0].kind, AnnotationKind::kText);
  EXPECT_EQ((*list)[1].kind, AnnotationKind::kOther);  // Popup
  EXPECT_FALSE((*list)[1].editable);
  auto snap = doc->snapshot_annotation(0, "#0");
  ASSERT_TRUE(snap);
  ASSERT_TRUE(doc->remove_annotation(0, snap->id));
  EXPECT_EQ(doc->annotations(0)->size(), 0u) << "Popup 도 함께 지워진다";
  ASSERT_TRUE(doc->restore_annotation(0, *snap));
  auto back = doc->annotations(0);
  ASSERT_EQ(back->size(), 1u);
  EXPECT_EQ((*back)[0].kind, AnnotationKind::kText);
  EXPECT_EQ((*back)[0].contents, "팝업이 있는 메모");
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

// 도형(#39): /Border 폭·/IC·불투명도가 쓰이고, 회전 쪽에서도 Rect 가 입력한 쪽 포인트로 돌아온다
TEST(Annotations, SquareAndCircleCarryBorderAndInterior) {
  for (int rotation : {0, 1}) {
    auto doc = PdfiumDocument::open(testdata("simple.pdf"));
    ASSERT_TRUE(doc);
    if (rotation != 0) ASSERT_TRUE(doc->set_page_rotation(0, rotation));
    AnnotationSpec square;
    square.kind = AnnotationKind::kSquare;
    square.rect = {100, 150, 200, 80};
    square.border_width = 4.0;
    square.color = {255, 0, 0, 128};
    square.interior = Color{255, 230, 230, 255};
    auto sid = doc->add_annotation(0, square);
    ASSERT_TRUE(sid) << sid.error().message;
    AnnotationSpec circle;
    circle.kind = AnnotationKind::kCircle;
    circle.rect = {320, 150, 120, 120};
    circle.border_width = 1.0;
    circle.color = {0, 0, 255, 255};
    auto cid = doc->add_annotation(0, circle);
    ASSERT_TRUE(cid) << cid.error().message;

    auto list = doc->annotations(0);
    ASSERT_TRUE(list);
    const AnnotationInfo* s = find_id(*list, *sid);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->kind, AnnotationKind::kSquare);
    EXPECT_NEAR(s->rect.x, 100, 0.01) << "rotation " << rotation;
    EXPECT_NEAR(s->rect.y, 150, 0.01);
    EXPECT_NEAR(s->rect.w, 200, 0.01);
    EXPECT_NEAR(s->rect.h, 80, 0.01);
    EXPECT_TRUE(s->has_border);
    EXPECT_FLOAT_EQ(s->border_width, 4.0f);
    EXPECT_NEAR(s->opacity, 128.0 / 255.0, 0.01);
    EXPECT_FALSE(s->has_bs);
    EXPECT_TRUE(s->editable);
    auto snap = doc->snapshot_annotation(0, *sid);
    ASSERT_TRUE(snap);
    EXPECT_TRUE(snap->has_interior);
    EXPECT_EQ(snap->interior, (Color{255, 230, 230, 128})) << "내부색 알파 = /CA";
    EXPECT_EQ(snap->color, (Color{255, 0, 0, 128}));
    const AnnotationInfo* c = find_id(*list, *cid);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->kind, AnnotationKind::kCircle);
    EXPECT_FLOAT_EQ(c->border_width, 1.0f);
    EXPECT_FLOAT_EQ(c->opacity, 1.0f);
    EXPECT_FALSE(doc->snapshot_annotation(0, *cid)->has_interior)
        << "/IC 가 없으면 채움 없음 (GetColor 의 기본 검정에 속지 않는다)";
    // 렌더: 사각형 안쪽이 채움(반투명 연분홍)으로 흰색이 아니다. 원 안쪽은 채움이 없어 흰색이
    // 남는다
    RenderOptions options;
    options.scale = 1.0f;
    auto bmp = doc->render_page(0, options);
    ASSERT_TRUE(bmp);
    const auto not_white = [](int r, int g, int b) { return !(r > 250 && g > 250 && b > 250); };
    EXPECT_GT(fraction_in(*bmp, {106, 156, 188, 68}, 1.0, not_white), 0.95)
        << "rotation " << rotation;
    EXPECT_LT(fraction_in(*bmp, {360, 190, 40, 40}, 1.0, not_white), 0.5)
        << "원 가운데는 채움이 없다";
  }
  AnnotationSpec no_rect;
  no_rect.kind = AnnotationKind::kSquare;
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  EXPECT_FALSE(doc->add_annotation(0, no_rect));
}

TEST(Annotations, BorderWidthSetterClearsApAndRejectsBs) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  AnnotationSpec square;
  square.kind = AnnotationKind::kSquare;
  square.rect = {100, 100, 100, 100};
  square.border_width = 2.0;
  auto id = doc->add_annotation(0, square);
  ASSERT_TRUE(id);
  RenderOptions options;
  options.scale = 0.5f;
  ASSERT_TRUE(doc->render_page(0, options));  // AP 생성
  EXPECT_FALSE(find_id(*doc->annotations(0), *id)->has_color) << "AP 가 있으면 색을 못 읽는다";
  ASSERT_TRUE(doc->set_annotation_border_width(0, *id, 6.0));
  auto snap = doc->snapshot_annotation(0, *id);
  ASSERT_TRUE(snap);
  EXPECT_FLOAT_EQ(snap->border_width, 6.0f);
  EXPECT_FALSE(doc->set_annotation_border_width(0, *id, 0.0));
  // 다른 프로그램이 /BS /W 로 만든 사각형: has_bs, setter 거절 (Command 가 삭제·재생성으로
  // 우회한다)
  auto other = PdfiumDocument::open(testdata("annotations.pdf"));
  ASSERT_TRUE(other);
  auto list = other->annotations(0);
  ASSERT_TRUE(list);
  ASSERT_EQ((*list)[1].kind, AnnotationKind::kSquare);
  EXPECT_TRUE((*list)[1].has_bs);
  EXPECT_FALSE((*list)[1].has_border) << "/BS 만 있고 /Border 는 없다";
  EXPECT_NEAR((*list)[1].opacity, 0.8, 0.001) << "/CA 는 AP 와 무관하게 읽힌다";
  auto rejected = other->set_annotation_border_width(0, "#1", 5.0);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code, ErrorCode::kInvalidArgument);
  EXPECT_TRUE(other->snapshot_annotation(0, "#1")->has_bs);
}

// 렌더 없이 저장해도 OnPdf 주석에는 AP 가 들어간다 — 없으면 다른 뷰어가 /CA 등을 무시하고 제멋대로
// 그린다 (#53)
TEST(Annotations, SaveGeneratesAppearanceForUnrenderedAnnotations) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  std::vector<std::string> ids;
  for (AnnotationKind kind :
       {AnnotationKind::kHighlight, AnnotationKind::kUnderline, AnnotationKind::kStrikeOut,
        AnnotationKind::kText, AnnotationKind::kInk, AnnotationKind::kSquare,
        AnnotationKind::kCircle}) {
    AnnotationSpec s;
    s.kind = kind;
    s.quads = {{72, 60, 200, 20}};
    s.rect = {300, 300, 60, 40};
    s.ink = {{{300, 300}, {330, 330}, {360, 300}}};
    s.border_width = 2;
    s.color = {0, 120, 0, 160};
    auto id = doc->add_annotation(1, s);  // 2쪽 (렌더하지 않는다)
    ASSERT_TRUE(id) << static_cast<int>(kind);
    ids.push_back(*id);
  }
  for (const auto& a : *doc->annotations(1))
    EXPECT_FALSE(a.has_appearance) << a.id << " — 새 주석은 AP 없이 만든다";
  ScopedTempDir dir;
  ASSERT_TRUE(save_document_safely(*doc, dir / "ap.pdf", {}));
  auto reopened = PdfiumDocument::open(dir / "ap.pdf");
  ASSERT_TRUE(reopened);
  const auto saved = *reopened->annotations(1);
  ASSERT_EQ(saved.size(), ids.size());
  for (const auto& a : saved) EXPECT_TRUE(a.has_appearance) << a.id;
  // 색을 바꾸면 AP 를 지운다 → 다음 저장이 다시 만든다
  ASSERT_TRUE(doc->set_annotation_color(1, ids[5], {200, 0, 0, 255}));
  ASSERT_TRUE(save_document_safely(*doc, dir / "ap2.pdf", {}));
  auto again = PdfiumDocument::open(dir / "ap2.pdf");
  ASSERT_TRUE(again);
  for (const auto& a : *again->annotations(1)) EXPECT_TRUE(a.has_appearance) << a.id;
  EXPECT_EQ(*doc->ensure_annotation_appearances(), 0) << "이미 AP 가 있으면 렌더하지 않는다";
}

// 저장 파일의 주석은 간접 객체다 (PDF 규격 표 30). FPDFPage_CreateAnnot 은 직접 사전을 넣지만, 저장
// 전 AP 생성 렌더가 간접 객체로 바꾼다 (#53). 렌더하지 않은 쪽·암호 문서도
TEST(Annotations, SavedAnnotationsAreIndirectObjects) {
  struct Case {
    const char* file;
    const char* password;
  };
  for (const Case c : {Case{"simple.pdf", ""}, Case{"encrypted-aes256.pdf", "owner"}}) {
    auto doc = PdfiumDocument::open(testdata(c.file), c.password);
    ASSERT_TRUE(doc) << c.file;
    AnnotationSpec s;
    s.quads = {{72, 60, 200, 20}};
    ASSERT_TRUE(doc->add_annotation(0, s));
    AnnotationSpec square;
    square.kind = AnnotationKind::kSquare;
    square.rect = {100, 300, 80, 40};
    ASSERT_TRUE(doc->add_annotation(0, square));
    ScopedTempDir dir;
    ASSERT_TRUE(save_document_safely(*doc, dir / "safe.pdf", {})) << c.file;
    auto direct = QpdfTools::direct_annotation_count(dir / "safe.pdf", c.password);
    ASSERT_TRUE(direct) << direct.error().message;
    EXPECT_EQ(*direct, 0) << c.file;
    auto reopened = PdfiumDocument::open(dir / "safe.pdf", c.password);
    ASSERT_TRUE(reopened) << c.file;
    for (const auto& a : *reopened->annotations(0))
      EXPECT_TRUE(a.has_appearance) << c.file << " " << a.id;
  }
  // 증분 저장(서명 문서)도 같다
  auto signed_doc = PdfiumDocument::open(testdata("signed-dummy.pdf"));
  ASSERT_TRUE(signed_doc);
  AnnotationSpec hl;
  hl.quads = {{72, 100, 200, 20}};
  ASSERT_TRUE(signed_doc->add_annotation(0, hl));
  ScopedTempDir dir;
  SaveOptions incremental;
  incremental.incremental = true;
  ASSERT_TRUE(save_document_safely(*signed_doc, dir / "signed.pdf", incremental));
  EXPECT_EQ(*QpdfTools::direct_annotation_count(dir / "signed.pdf"), 0);
}

}  // namespace
}  // namespace onpdf::engine
