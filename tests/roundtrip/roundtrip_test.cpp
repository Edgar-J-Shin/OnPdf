// 왕복 테스트: manifest 의 열 수 있는 모든 PDF 를 열기 → (변경 없이) 안전 저장 → 다시 열기 후 일치 확인.
// 계획서 성공 기준 "테스트 PDF 열기·저장 왕복 후 깨지는 파일 0건". 구조 검사는 qpdf 라이브러리(QpdfTools::check)로 항상 돈다.

#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/qpdf_tools.h"
#include "onpdf/core/engine/save.h"
#include "onpdf/edit/add_text_command.h"
#include "onpdf/edit/annotation_commands.h"
#include "onpdf/edit/delete_pages.h"
#include "onpdf/edit/insert_pages.h"
#include "onpdf/edit/move_pages.h"

#include "../manifest.h"
#include "../test_support.h"

#include <gtest/gtest.h>

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

  // 라이브러리 qpdf 로 구조 검사 (CI 에도 항상 돈다). 경고(원본에도 있던 경고 포함)는 통과
  auto checked = engine::QpdfTools::check(target, e.password);
  EXPECT_TRUE(checked) << "qpdf check: " << (checked ? "" : checked.error().message);
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


// 저장 파일에 qpdf 라이브러리 검사를 돌린다 (경고는 통과, 오류는 실패)
void expect_qpdf_ok(const ScopedTempDir&, const std::filesystem::path& target) {
  auto checked = engine::QpdfTools::check(target);
  EXPECT_TRUE(checked) << "qpdf check: " << (checked ? "" : checked.error().message);
}

std::string page_text(const PdfiumDocument& doc, int page) {
  auto text = doc.load_text(page);
  return text ? text->all_text() : std::string();
}

// 쪽 삭제·이동 뒤 저장: 순서가 파일에 남고, 목차가 깨지지 않는다
TEST(Roundtrip, DeletedAndMovedDocument) {
  auto doc = PdfiumDocument::open(testdata("bookmarks.pdf"));
  ASSERT_TRUE(doc);
  ASSERT_EQ(doc->page_count(), 10);
  edit::DeletePagesCommand del({0});
  ASSERT_TRUE(del.execute(*doc));
  edit::MovePagesCommand mv({8}, 0);  // 이제 마지막 쪽(원래 10쪽)을 맨 앞으로
  ASSERT_TRUE(mv.execute(*doc));
  ScopedTempDir dir;
  const auto target = dir / "pages.pdf";
  ASSERT_TRUE(save_document_safely(*doc, target, {}));
  auto reopened = PdfiumDocument::open(target);
  ASSERT_TRUE(reopened);
  ASSERT_EQ(reopened->page_count(), 9);
  EXPECT_NE(page_text(*reopened, 0).find("page 10 of 10"), std::string::npos) << page_text(*reopened, 0);
  EXPECT_NE(page_text(*reopened, 1).find("page 2 of 10"), std::string::npos) << page_text(*reopened, 1);
  EXPECT_NE(page_text(*reopened, 8).find("page 9 of 10"), std::string::npos) << page_text(*reopened, 8);
  auto outline = reopened->load_outline();
  ASSERT_TRUE(outline) << outline.error().message;
  EXPECT_GT(outline->size(), 0);
  for (const auto& item : outline->items) {
    if (item.target && item.target->kind == view::LinkKind::kPage) EXPECT_LT(item.target->page, 9) << item.title;
  }
  expect_qpdf_ok(dir, target);
}

// 다른 파일(한글·폼)의 쪽을 삽입하고 저장: 쪽 수·텍스트가 남고 qpdf 검사를 통과한다
TEST(Roundtrip, InsertedFromAnotherFile) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  edit::InsertPagesCommand korean(testdata("korean-embedded.pdf"), "", {}, 2);
  ASSERT_TRUE(korean.execute(*doc));
  edit::InsertPagesCommand form(testdata("form-acro.pdf"), "", {}, 0);
  ASSERT_TRUE(form.execute(*doc));
  ASSERT_EQ(doc->page_count(), 4);
  const std::string korean_text = page_text(*doc, 3);
  ScopedTempDir dir;
  const auto target = dir / "inserted.pdf";
  ASSERT_TRUE(save_document_safely(*doc, target, {}));
  auto reopened = PdfiumDocument::open(target);
  ASSERT_TRUE(reopened);
  ASSERT_EQ(reopened->page_count(), 4);
  EXPECT_EQ(page_text(*reopened, 3), korean_text);
  EXPECT_FALSE(korean_text.empty());
  for (int i = 0; i < 4; ++i) {
    RenderOptions render;
    render.scale = 0.25f;
    EXPECT_TRUE(reopened->render_page(i, render)) << "page " << i;
  }
  expect_qpdf_ok(dir, target);
}

// 서명 문서에 쪽을 삽입하고 증분 저장: 원본 바이트가 앞부분에 그대로 남는다 (서명 보존 규칙)
TEST(Roundtrip, SignedDocumentInsertSavesIncrementally) {
  auto doc = PdfiumDocument::open(testdata("signed-dummy.pdf"));
  ASSERT_TRUE(doc);
  ASSERT_GT(doc->signature_count(), 0);
  edit::InsertPagesCommand cmd(testdata("simple.pdf"), "", {0}, 1);
  ASSERT_TRUE(cmd.execute(*doc));
  ScopedTempDir dir;
  const auto target = dir / "signed-inserted.pdf";
  SaveOptions options;
  options.incremental = true;
  auto saved = save_document_safely(*doc, target, options);
  ASSERT_TRUE(saved) << saved.error().message;
  const auto original = onpdf::test::read_bytes(testdata("signed-dummy.pdf"));
  const auto after = onpdf::test::read_bytes(target);
  ASSERT_GT(after.size(), original.size());
  EXPECT_TRUE(std::equal(original.begin(), original.end(), after.begin()));
  auto reopened = PdfiumDocument::open(target);
  ASSERT_TRUE(reopened);
  EXPECT_EQ(reopened->page_count(), 2);
  expect_qpdf_ok(dir, target);
}


const engine::AnnotationInfo* annot_by_id(const std::vector<engine::AnnotationInfo>& list, const std::string& id) {
  for (const auto& a : list) if (a.id == id) return &a;
  return nullptr;
}

// 주석 5종을 만들어 저장 → 다시 열기: 수·종류·위치·내용(한글)이 남고 qpdf 검사를 통과한다
TEST(Roundtrip, AnnotationsSurviveSave) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  auto text = doc->load_text(0);
  ASSERT_TRUE(text);
  const std::vector<Rect> line = text->selection_rects({0, std::min(12, text->char_count())});
  ASSERT_FALSE(line.empty());
  std::vector<std::pair<int, engine::AnnotationSpec>> items;
  for (auto kind : {engine::AnnotationKind::kHighlight, engine::AnnotationKind::kUnderline, engine::AnnotationKind::kStrikeOut}) {
    engine::AnnotationSpec s;
    s.kind = kind;
    s.quads = line;
    items.emplace_back(0, s);
  }
  engine::AnnotationSpec note;
  note.kind = engine::AnnotationKind::kText;
  note.rect = {400, 100, 20, 20};
  note.flags = engine::kAnnotFlagPrint | engine::kAnnotFlagNoZoom | engine::kAnnotFlagNoRotate;
  note.contents = "검토 필요: 한글 메모";
  note.author = "테스터";
  items.emplace_back(1, note);
  engine::AnnotationSpec ink;
  ink.kind = engine::AnnotationKind::kInk;
  ink.ink = {{{100, 300}, {140, 280}, {180, 300}, {220, 280}}};
  ink.rect = {98, 278, 124, 24};
  ink.border_width = 2.0;
  ink.color = {255, 0, 0, 255};
  items.emplace_back(1, ink);
  edit::AddAnnotationsCommand add(items);
  ASSERT_TRUE(add.execute(*doc));
  // 렌더해 AP 가 생긴 상태로 저장한다 (실제 앱 흐름)
  RenderOptions render;
  render.scale = 0.5f;
  ASSERT_TRUE(doc->render_page(0, render));
  ASSERT_TRUE(doc->render_page(1, render));
  ScopedTempDir dir;
  const auto target = dir / "annotated.pdf";
  ASSERT_TRUE(save_document_safely(*doc, target, {}));
  auto reopened = PdfiumDocument::open(target);
  ASSERT_TRUE(reopened);
  auto p0 = reopened->annotations(0);
  auto p1 = reopened->annotations(1);
  ASSERT_TRUE(p0 && p1);
  ASSERT_EQ(p0->size(), 3u);
  ASSERT_EQ(p1->size(), 2u);
  for (std::size_t i = 0; i < 3; ++i) {
    const auto* a = annot_by_id(*p0, add.items()[i].second.id);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->kind, add.items()[i].second.kind);
    EXPECT_NEAR(a->rect.x, line.front().x, 0.05);
    EXPECT_EQ(a->flags & engine::kAnnotFlagPrint, engine::kAnnotFlagPrint);
  }
  const auto* n = annot_by_id(*p1, add.items()[3].second.id);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->contents, "검토 필요: 한글 메모");
  EXPECT_EQ(n->author, "테스터");
  EXPECT_NEAR(n->rect.x, 400, 0.05);
  const auto* k = annot_by_id(*p1, add.items()[4].second.id);
  ASSERT_NE(k, nullptr);
  EXPECT_EQ(k->kind, engine::AnnotationKind::kInk);
  expect_qpdf_ok(dir, target);
}

// 기존 문서의 주석을 고치고(색·삭제) 저장해도 나머지 주석·링크가 그대로다
// 도형(#39): 추가 + 두께·불투명도 변경 뒤 저장 → 다시 열기 → 종류·Rect·/Border·/CA·채움 일치
TEST(Roundtrip, ShapesSurviveSave) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  engine::AnnotationSpec square;
  square.kind = engine::AnnotationKind::kSquare;
  square.rect = {100, 100, 200, 80};
  square.border_width = 2.0;
  square.color = {255, 0, 0, 255};
  square.interior = engine::Color{255, 230, 230, 255};
  engine::AnnotationSpec circle;
  circle.kind = engine::AnnotationKind::kCircle;
  circle.rect = {320, 100, 120, 120};
  circle.border_width = 2.0;
  circle.color = {0, 0, 255, 255};
  edit::AddAnnotationsCommand add({{0, square}, {1, circle}});
  ASSERT_TRUE(add.execute(*doc));
  RenderOptions render;
  render.scale = 0.5f;
  ASSERT_TRUE(doc->render_page(0, render));  // AP 가 생긴 상태에서 속성 변경 (앱 흐름)
  const std::string sid = (*doc->annotations(0))[0].id;
  edit::SetAnnotationBorderWidthCommand width(0, sid, 6.0);
  ASSERT_TRUE(width.execute(*doc));
  edit::SetAnnotationOpacityCommand opacity(0, sid, 128);
  ASSERT_TRUE(opacity.execute(*doc));
  ASSERT_TRUE(doc->render_page(0, render));
  ASSERT_TRUE(doc->render_page(1, render));
  ScopedTempDir dir;
  const auto target = dir / "shapes.pdf";
  ASSERT_TRUE(save_document_safely(*doc, target, {}));
  auto reopened = PdfiumDocument::open(target);
  ASSERT_TRUE(reopened);
  auto p0 = reopened->annotations(0);
  auto p1 = reopened->annotations(1);
  ASSERT_TRUE(p0 && p1);
  ASSERT_EQ(p0->size(), 1u);
  ASSERT_EQ(p1->size(), 1u);
  const auto& s = (*p0)[0];
  EXPECT_EQ(s.kind, engine::AnnotationKind::kSquare);
  EXPECT_EQ(s.id, sid);
  EXPECT_NEAR(s.rect.x, 100, 0.5);
  EXPECT_NEAR(s.rect.w, 200, 0.5);
  EXPECT_NEAR(s.rect.h, 80, 0.5);
  EXPECT_FLOAT_EQ(s.border_width, 6.0f);
  EXPECT_NEAR(s.opacity, 128.0 / 255.0, 0.01);
  EXPECT_TRUE(s.flags & engine::kAnnotFlagPrint);
  auto snap = reopened->snapshot_annotation(0, sid);
  ASSERT_TRUE(snap);
  EXPECT_EQ(snap->color, (engine::Color{255, 0, 0, 128}));
  EXPECT_TRUE(snap->has_interior);
  EXPECT_EQ(snap->interior, (engine::Color{255, 230, 230, 128})) << "내부색 알파 = /CA";
  const auto& c = (*p1)[0];
  EXPECT_EQ(c.kind, engine::AnnotationKind::kCircle);
  EXPECT_NEAR(c.rect.x, 320, 0.5);
  EXPECT_FLOAT_EQ(c.border_width, 2.0f);
  EXPECT_FLOAT_EQ(c.opacity, 1.0f);
  auto check = engine::QpdfTools::check(target, "");
  EXPECT_TRUE(check) << (check ? "" : check.error().message);
}

// 텍스트 추가(#45): 저장 → 다시 열기(내장 폰트만) → 추출 == 입력, 렌더 == 저장 전, 마크 id 보존, qpdf. 평문·회전·서명(증분)·암호
TEST(Roundtrip, AddedTextSurvivesSave) {
  auto face = edit::FontFace::load(testdata("fonts") / "NotoSansKR-subset.ttf");
  ASSERT_TRUE(face);
  struct Case { const char* file; const char* password; bool rotate; bool incremental; };
  const Case cases[] = {{"simple.pdf", "", false, false}, {"simple.pdf", "", true, false},
                        {"signed-dummy.pdf", "", false, true}, {"encrypted-aes256.pdf", "owner", false, false}};
  for (const Case& c : cases) {
    auto doc = PdfiumDocument::open(testdata(c.file), c.password);
    ASSERT_TRUE(doc) << c.file;
    if (c.rotate) ASSERT_TRUE(doc->set_page_rotation(0, 1));
    const std::string text = "한글 텍스트 추가 검증 임베딩 왕복\nOnPdf 2026 (M3)";
    edit::TextBoxSpec spec;
    spec.page = 0;
    spec.origin = {60, 120};
    spec.size = 18;
    spec.utf8 = text;
    spec.face = *face;
    edit::AddTextCommand add(spec);
    ASSERT_TRUE(add.execute(*doc)) << c.file;
    RenderOptions render;
    render.scale = 1.0f;
    auto before = doc->render_page(0, render);
    ASSERT_TRUE(before);
    const auto before_objects = *doc->text_objects(0);
    ScopedTempDir dir;
    const auto target = dir / "text.pdf";
    engine::SaveOptions options;
    options.incremental = c.incremental;
    auto saved = save_document_safely(*doc, target, options);
    ASSERT_TRUE(saved) << c.file << ": " << saved.error().message;
    auto reopened = PdfiumDocument::open(target, c.password);
    ASSERT_TRUE(reopened) << c.file;
    auto index = reopened->load_text(0);
    ASSERT_TRUE(index);
    EXPECT_NE(index->all_text().find("한글 텍스트 추가 검증 임베딩 왕복"), std::string::npos) << c.file << ": " << index->all_text();
    EXPECT_NE(index->all_text().find("OnPdf 2026 (M3)"), std::string::npos) << c.file;
    auto after = reopened->render_page(0, render);
    ASSERT_TRUE(after);
    ASSERT_EQ(after->pixels.size(), before->pixels.size());
    int diff = 0;
    for (std::size_t i = 0; i < before->pixels.size(); ++i) diff += before->pixels[i] != after->pixels[i];
    EXPECT_EQ(diff, 0) << c.file << ": 저장 전후 렌더가 같아야 한다";
    const auto after_objects = *reopened->text_objects(0);
    ASSERT_EQ(after_objects.size(), before_objects.size()) << c.file;
    for (std::size_t i = 0; i < before_objects.size(); ++i) EXPECT_EQ(after_objects[i].id, before_objects[i].id);
    auto check = engine::QpdfTools::check(target, c.password);
    EXPECT_TRUE(check) << c.file << ": " << (check ? "" : check.error().message);
    if (c.incremental) EXPECT_GT(reopened->signature_count(), 0) << "서명이 보존된다";
  }
}

TEST(Roundtrip, EditingExistingAnnotationsKeepsOthers) {
  auto doc = PdfiumDocument::open(testdata("annotations.pdf"));
  ASSERT_TRUE(doc);
  edit::SetAnnotationColorCommand color(0, "#0", {0, 255, 0, 255});
  ASSERT_TRUE(color.execute(*doc));
  edit::RemoveAnnotationCommand remove(0, "#1");  // Square
  ASSERT_TRUE(remove.execute(*doc));
  RenderOptions render;
  render.scale = 0.5f;
  ASSERT_TRUE(doc->render_page(0, render));
  ScopedTempDir dir;
  const auto target = dir / "edited.pdf";
  ASSERT_TRUE(save_document_safely(*doc, target, {}));
  auto reopened = PdfiumDocument::open(target);
  ASSERT_TRUE(reopened);
  auto list = reopened->annotations(0);
  ASSERT_TRUE(list);
  ASSERT_EQ(list->size(), 5u);
  int links = 0, highlights = 0, squares = 0;
  for (const auto& a : *list) {
    links += a.kind == engine::AnnotationKind::kLink;
    highlights += a.kind == engine::AnnotationKind::kHighlight;
    squares += a.kind == engine::AnnotationKind::kSquare;
  }
  EXPECT_EQ(links, 3);
  EXPECT_EQ(highlights, 1);
  EXPECT_EQ(squares, 0);
  auto text = reopened->load_text(0);
  ASSERT_TRUE(text);
  EXPECT_EQ(text->char_count(), 222);  // 본문 텍스트 불변
  expect_qpdf_ok(dir, target);
}

// 서명 문서에 메모를 달고 증분 저장: 원본 바이트가 앞부분에 그대로
TEST(Roundtrip, SignedDocumentNoteSavesIncrementally) {
  auto doc = PdfiumDocument::open(testdata("signed-dummy.pdf"));
  ASSERT_TRUE(doc);
  const std::size_t before_count = doc->annotations(0)->size();  // 서명 위젯 주석이 이미 있다
  engine::AnnotationSpec note;
  note.kind = engine::AnnotationKind::kText;
  note.rect = {50, 50, 20, 20};
  note.contents = "서명 문서 메모";
  edit::AddAnnotationsCommand add({{0, note}});
  ASSERT_TRUE(add.execute(*doc));
  ScopedTempDir dir;
  const auto target = dir / "signed-note.pdf";
  SaveOptions options;
  options.incremental = true;
  ASSERT_TRUE(save_document_safely(*doc, target, options));
  const auto original = onpdf::test::read_bytes(testdata("signed-dummy.pdf"));
  const auto after = onpdf::test::read_bytes(target);
  ASSERT_GT(after.size(), original.size());
  EXPECT_TRUE(std::equal(original.begin(), original.end(), after.begin()));
  auto reopened = PdfiumDocument::open(target);
  ASSERT_TRUE(reopened);
  auto list = reopened->annotations(0);
  ASSERT_TRUE(list);
  ASSERT_EQ(list->size(), before_count + 1);
  const auto* n = annot_by_id(*list, add.items()[0].second.id);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->contents, "서명 문서 메모");
  expect_qpdf_ok(dir, target);
}


// 병합 결과를 PDFium 으로 다시 열어 쪽 순서(텍스트)·첫 쪽 렌더·목차가 입력과 같다 (#37)
TEST(Roundtrip, MergedDocumentMatchesInputs) {
  ScopedTempDir dir;
  const auto out = dir / "merged.pdf";
  auto report = engine::QpdfTools::merge({{testdata("bookmarks.pdf"), ""}, {testdata("simple.pdf"), ""}}, out);
  ASSERT_TRUE(report) << report.error().message;
  auto merged = PdfiumDocument::open(out);
  auto bookmarks = PdfiumDocument::open(testdata("bookmarks.pdf"));
  auto simple = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(merged && bookmarks && simple);
  ASSERT_EQ(merged->page_count(), 12);
  EXPECT_EQ(page_text(*merged, 0), page_text(*bookmarks, 0));
  EXPECT_EQ(page_text(*merged, 9), page_text(*bookmarks, 9));
  EXPECT_EQ(page_text(*merged, 10), page_text(*simple, 0));
  EXPECT_EQ(page_text(*merged, 11), page_text(*simple, 1));
  EXPECT_EQ(merged->load_outline()->size(), 15);
  RenderOptions render;
  render.scale = 0.5f;
  auto a = merged->render_page(10, render);
  auto b = simple->render_page(0, render);
  ASSERT_TRUE(a && b);
  EXPECT_LE(different_fraction(*a, *b), onpdf::test::kMaxDifferentFraction);
  expect_qpdf_ok(dir, out);
}

TEST(Roundtrip, SplitPiecesMatchSource) {
  ScopedTempDir dir;
  const engine::StructureInput in{testdata("mixed-sizes.pdf"), ""};
  auto source = PdfiumDocument::open(in.path);
  ASSERT_TRUE(source);
  const auto ranges = onpdf::ranges_every(3, source->page_count());  // 7쪽 → 3개
  ASSERT_EQ(ranges.size(), 3u);
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    const auto out = dir.path() / ("piece-" + std::to_string(i) + ".pdf");
    ASSERT_TRUE(engine::QpdfTools::extract(in, ranges[i], out));
    auto piece = PdfiumDocument::open(out);
    ASSERT_TRUE(piece);
    ASSERT_EQ(piece->page_count(), ranges[i].last - ranges[i].first + 1);
    for (int p = 0; p < piece->page_count(); ++p) {
      const int src = ranges[i].first - 1 + p;
      EXPECT_NEAR(piece->page_size(p)->width, source->page_size(src)->width, 0.01) << "piece " << i << " page " << p;
      EXPECT_EQ(page_text(*piece, p), page_text(*source, src));
    }
    expect_qpdf_ok(dir, out);
  }
}

}  // namespace
}  // namespace onpdf::engine
