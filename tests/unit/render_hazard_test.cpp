// 렌더 위험 (#121): qpdf 검사기가 타일링 패턴 폭탄을 쪽·Form XObject·주석 AP 에서 찾고, 엔진이 그
// 쪽의 렌더를 PDFium 호출 없이 즉시 거절하며, 쪽 구조 편집·스냅샷·바이트 교체 뒤에도 판정이 쪽을
// 따라가는지. pattern-bomb.pdf: 0 정상(36pt 패턴), 1 폭탄, 2 Form 안 폭탄, 3 주석 AP 안 폭탄

#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/qpdf_tools.h"
#include "onpdf/core/engine/save.h"

#include "../test_support.h"
#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <vector>

namespace onpdf::engine {
namespace {

using onpdf::test::ScopedTempDir;
using onpdf::test::testdata;

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<int> hazard_pages(const std::vector<RenderHazard>& hazards) {
  std::vector<int> pages;
  for (const auto& h : hazards) pages.push_back(h.page);
  return pages;
}

std::vector<int> refused_pages(const PdfiumDocument& doc) {
  std::vector<int> pages;
  RenderOptions o;
  o.scale = 0.25f;
  for (int i = 0; i < doc.page_count(); ++i) {
    auto r = doc.render_page(i, o);
    if (!r && r.error().code == ErrorCode::kRenderRefused) pages.push_back(i);
  }
  return pages;
}

TEST(RenderHazardScan, FindsBombsInPageFormAndAnnotation) {
  auto hazards = QpdfTools::scan_render_hazards(read_bytes(testdata("pattern-bomb.pdf")));
  ASSERT_TRUE(hazards) << hazards.error().message;
  EXPECT_EQ(hazard_pages(*hazards), (std::vector<int>{1, 2, 3}));
  for (const auto& h : *hazards) {
    EXPECT_EQ(h.kind, RenderHazardKind::kTilingPatternTooDense);
    EXPECT_GT(h.estimate, kMaxPatternTilesPerPage);
    EXPECT_NE(h.detail.find("XStep 0.001"), std::string::npos);
  }
}

TEST(RenderHazardScan, OrdinaryDocumentsHaveNone) {
  for (const char* name : {"simple.pdf", "transparency.pdf", "annotations.pdf", "form-fields.pdf",
                           "text-lines-form.pdf"}) {
    auto hazards = QpdfTools::scan_render_hazards(read_bytes(testdata(name)));
    ASSERT_TRUE(hazards) << name << ": " << hazards.error().message;
    EXPECT_TRUE(hazards->empty()) << name;
  }
}

TEST(RenderHazardScan, EncryptedNeedsPasswordAndDamagedFails) {
  const auto encrypted = read_bytes(testdata("encrypted-rc4-user.pdf"));
  auto with = QpdfTools::scan_render_hazards(encrypted, "user");
  ASSERT_TRUE(with) << with.error().message;
  EXPECT_TRUE(with->empty());
  auto without = QpdfTools::scan_render_hazards(encrypted);
  ASSERT_FALSE(without);
  EXPECT_EQ(without.error().code, ErrorCode::kPasswordRequired);
  auto damaged = QpdfTools::scan_render_hazards(read_bytes(testdata("truncated.pdf")));
  EXPECT_FALSE(damaged);
}

TEST(RenderHazard, RefusesBombPagesQuicklyAndRendersSafePage) {
  auto doc = PdfiumDocument::open(testdata("pattern-bomb.pdf"));
  ASSERT_TRUE(doc) << doc.error().message;
  EXPECT_EQ(hazard_pages(doc->render_hazards()), (std::vector<int>{1, 2, 3}));

  const auto start = std::chrono::steady_clock::now();
  RenderOptions o;
  o.scale = 0.5f;
  auto refused = doc->render_page(1, o);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code, ErrorCode::kRenderRefused);
  auto region = doc->render_region(2, o, Rect{0, 0, 64, 64});
  ASSERT_FALSE(region);
  EXPECT_EQ(region.error().code, ErrorCode::kRenderRefused);
  auto annot_page = doc->render_page(3, o);
  ASSERT_FALSE(annot_page);
  EXPECT_EQ(annot_page.error().code, ErrorCode::kRenderRefused);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(elapsed, std::chrono::seconds(2)) << "refusal must not touch PDFium rendering";

  auto safe = doc->render_page(0, o);
  ASSERT_TRUE(safe) << safe.error().message;
  bool has_ink = false;
  for (int y = 0; y < safe->height && !has_ink; ++y) {
    const auto* row = safe->row(y);
    for (int x = 0; x < safe->width; ++x) {
      if (row[x * 4] != 0xFF || row[x * 4 + 1] != 0xFF || row[x * 4 + 2] != 0xFF) {
        has_ink = true;
        break;
      }
    }
  }
  EXPECT_TRUE(has_ink);
  // 범위 밖 쪽은 기존 오류 그대로
  auto out = doc->render_page(9, o);
  ASSERT_FALSE(out);
  EXPECT_EQ(out.error().code, ErrorCode::kPageNotFound);
}

TEST(RenderHazard, FollowsDeleteMoveAndUndo) {
  auto doc = PdfiumDocument::open(testdata("pattern-bomb.pdf"));
  ASSERT_TRUE(doc);
  // 삭제 취소용 사본을 먼저 뜨고(판정 포함) 쪽 0 을 지우면 폭탄이 0·1·2 가 된다
  auto snapshot = doc->copy_pages_to_snapshot({0});
  ASSERT_TRUE(snapshot) << snapshot.error().message;
  ASSERT_TRUE(doc->delete_pages({0}));
  EXPECT_EQ(refused_pages(*doc), (std::vector<int>{0, 1, 2}));
  // 되돌리면 원래대로
  ASSERT_TRUE(doc->insert_from_snapshot(*snapshot, {}, 0));
  EXPECT_EQ(refused_pages(*doc), (std::vector<int>{1, 2, 3}));
  // 정상 쪽을 맨 뒤로 옮기면 폭탄이 0·1·2
  ASSERT_TRUE(doc->move_pages({0}, 3));
  EXPECT_EQ(refused_pages(*doc), (std::vector<int>{0, 1, 2}));
  // 폭탄 쪽(현재 0)을 사본으로 떠서 지우고 다시 넣어도 판정이 따라온다
  auto bomb_copy = doc->copy_pages_to_snapshot({0});
  ASSERT_TRUE(bomb_copy);
  ASSERT_TRUE(doc->delete_pages({0}));
  EXPECT_EQ(refused_pages(*doc), (std::vector<int>{0, 1}));
  ASSERT_TRUE(doc->insert_from_snapshot(*bomb_copy, {}, 3));
  EXPECT_EQ(refused_pages(*doc), (std::vector<int>{0, 1, 3}));
}

TEST(RenderHazard, InsertedFromFileCarriesVerdict) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  EXPECT_TRUE(doc->render_hazards().empty());
  auto source = doc->open_snapshot(testdata("pattern-bomb.pdf"), "");
  ASSERT_TRUE(source) << source.error().message;
  ASSERT_TRUE(doc->insert_from_snapshot(*source, {1, 0}, 1));  // 폭탄, 정상 순으로 쪽 1 에
  ASSERT_EQ(doc->page_count(), 4);
  EXPECT_EQ(refused_pages(*doc), (std::vector<int>{1}));
}

TEST(RenderHazard, SurvivesSaveAndReopen) {
  auto doc = PdfiumDocument::open(testdata("pattern-bomb.pdf"));
  ASSERT_TRUE(doc);
  ScopedTempDir dir;
  const auto target = dir / "bomb.pdf";
  auto saved = save_document_safely(*doc, target, {});  // 저장 검증은 쪽을 열 뿐 렌더하지 않는다
  ASSERT_TRUE(saved) << saved.error().message;
  auto reopened = PdfiumDocument::open(target);
  ASSERT_TRUE(reopened);
  EXPECT_EQ(hazard_pages(reopened->render_hazards()), (std::vector<int>{1, 2, 3}));
}

}  // namespace
}  // namespace onpdf::engine
