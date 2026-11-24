// 기존 텍스트 줄 수정 (#65) 엔진: 줄 묶기, qpdf 수술 삭제·교체(원본 글꼴·대체 글꼴), 줄 밖 보존,
// 상태 토큰, 떼어 둔 객체 이월, Form XObject, 암호·서명 문서, 거절 조건
#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/qpdf_tools.h"
#include "onpdf/core/engine/save.h"
#include "onpdf/edit/add_text_command.h"
#include "onpdf/edit/font_face.h"
#include "onpdf/edit/object_commands.h"
#include "onpdf/edit/text_box_commands.h"

#include "../test_support.h"
#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>

namespace onpdf::engine {
namespace {

using onpdf::test::ScopedTempDir;
using onpdf::test::testdata;

std::shared_ptr<const edit::FontFace> bundled_face() {
  auto face = edit::FontFace::load(testdata("fonts") / "NotoSansKR-subset.ttf");
  EXPECT_TRUE(face);
  return face ? *face : nullptr;
}

const TextLineInfo* find_line(const std::vector<TextLineInfo>& lines, const std::string& needle) {
  for (const auto& l : lines) {
    if (l.text.find(needle) != std::string::npos) return &l;
  }
  return nullptr;
}

TextObjectsSpec fallback_for(const TextLineInfo& line, const std::string& utf8) {
  edit::TextBoxSpec spec;
  spec.page = 0;
  spec.origin = {line.bbox.x, line.bbox.y};
  spec.size = line.font_size;
  spec.color = line.color;
  spec.utf8 = utf8;
  spec.face = bundled_face();
  auto built = edit::build_text_objects_spec(spec, make_text_box_id(), {});
  EXPECT_TRUE(built) << (built ? "" : built.error().message);
  return built ? *built : TextObjectsSpec{};
}

Bitmap render(const PdfiumDocument& doc, int page = 0) {
  RenderOptions o;
  o.scale = 1.0f;
  o.render_annotations = false;
  auto b = doc.render_page(page, o);
  EXPECT_TRUE(b);
  return b ? *b : Bitmap{};
}

// 쪽 포인트 사각형(여유 pad) 밖에서 다른 픽셀 수 (배율 1)
long diff_outside(const Bitmap& a, const Bitmap& b, const Rect& mask, double pad) {
  if (a.width != b.width || a.height != b.height) return -1;
  long n = 0;
  for (int y = 0; y < a.height; ++y) {
    for (int x = 0; x < a.width; ++x) {
      if (x >= mask.x - pad && x <= mask.right() + pad && y >= mask.y - pad &&
          y <= mask.bottom() + pad)
        continue;
      const auto* pa = a.row(y) + x * 4;
      const auto* pb = b.row(y) + x * 4;
      if (pa[0] != pb[0] || pa[1] != pb[1] || pa[2] != pb[2]) ++n;
    }
  }
  return n;
}

std::string page_text(const PdfiumDocument& doc, int page = 0) {
  auto t = doc.load_text(page);
  EXPECT_TRUE(t);
  return t ? t->all_text() : std::string();
}

TEST(TextLines, GroupsLinesAndFlagsFontsAndEditability) {
  auto doc = PdfiumDocument::open(testdata("text-lines-basic.pdf"));
  ASSERT_TRUE(doc);
  auto lines = doc->text_lines(0);
  ASSERT_TRUE(lines) << lines.error().message;
  EXPECT_EQ(lines->size(), 11u);
  const TextLineInfo* fox = find_line(*lines, "quick brown fox");
  ASSERT_NE(fox, nullptr);
  EXPECT_EQ(fox->font_name, "Helvetica");
  EXPECT_NEAR(fox->font_size, 14.0, 0.01);
  EXPECT_FALSE(fox->font_embedded) << "표준 14 글꼴은 임베딩이 아니다";
  EXPECT_TRUE(fox->editable);
  EXPECT_EQ(fox->object_count, 1);
  EXPECT_NEAR(fox->bbox.x, 72.0, 2.0);
  const TextLineInfo* mixed = find_line(*lines, "Bold start");
  ASSERT_NE(mixed, nullptr);
  EXPECT_TRUE(mixed->mixed_fonts);
  EXPECT_EQ(mixed->object_count, 2);
  EXPECT_NE(mixed->text.find("mixed line"), std::string::npos)
      << "두 객체가 한 줄로 묶인다: " << mixed->text;
  const TextLineInfo* spaced = find_line(*lines, "Spaced words");
  ASSERT_NE(spaced, nullptr);
  EXPECT_TRUE(spaced->editable);
  // id 는 같은 바이트에서 결정적
  auto again = doc->text_lines(0);
  ASSERT_TRUE(again);
  for (std::size_t i = 0; i < lines->size(); ++i) EXPECT_EQ((*lines)[i].id, (*again)[i].id);
  // 없는 줄
  auto missing = doc->line_font_covers(0, "p/99/0", "x");
  EXPECT_FALSE(missing);
}

TEST(TextLines, RemoveLinePreservesEverythingElse) {
  auto doc = PdfiumDocument::open(testdata("text-lines-basic.pdf"));
  ASSERT_TRUE(doc);
  const Bitmap before = render(*doc);
  auto lines = doc->text_lines(0);
  ASSERT_TRUE(lines);
  const TextLineInfo* target = find_line(*lines, "Delete me entirely");
  ASSERT_NE(target, nullptr);
  const Rect bbox = target->bbox;
  auto removed = doc->remove_text_line(0, target->id);
  ASSERT_TRUE(removed) << removed.error().message;
  EXPECT_NE(removed->before, 0u);
  EXPECT_EQ(doc->document_state_count(), 1);
  const std::string text = page_text(*doc);
  EXPECT_EQ(text.find("Delete me entirely"), std::string::npos);
  EXPECT_NE(text.find("quick brown fox"), std::string::npos);
  EXPECT_NE(text.find("Spaced words"), std::string::npos);
  auto after_lines = doc->text_lines(0);
  ASSERT_TRUE(after_lines);
  EXPECT_EQ(after_lines->size(), 10u);
  const Bitmap after = render(*doc);
  EXPECT_EQ(diff_outside(before, after, bbox, 5.0), 0) << "줄 밖은 픽셀 하나도 안 바뀐다";
  EXPECT_GT(diff_outside(before, after, Rect{0, 0, 0, 0}, 0.0), 100) << "줄 자체는 지워졌다";
  // 상태 복원 → 원래대로
  ASSERT_TRUE(doc->restore_document_state(removed->before));
  EXPECT_EQ(render(*doc).pixels, before.pixels);
  EXPECT_NE(page_text(*doc).find("Delete me entirely"), std::string::npos);
  doc->drop_document_state(removed->before);
  EXPECT_EQ(doc->document_state_count(), 0);
  // 저장 → 다시 열기 → qpdf check
  ASSERT_TRUE(doc->remove_text_line(0, target->id));
  ScopedTempDir dir;
  const auto out = dir / "removed.pdf";
  auto saved = save_document_safely(*doc, out, {});
  ASSERT_TRUE(saved) << saved.error().message;
  auto reopened = PdfiumDocument::open(out);
  ASSERT_TRUE(reopened);
  EXPECT_EQ(page_text(*reopened).find("Delete me entirely"), std::string::npos);
  auto check = QpdfTools::check(out);
  ASSERT_TRUE(check);
  EXPECT_TRUE(check->warnings.empty());
}

TEST(TextLines, ReplaceNonEmbeddedLineUsesFallbackFont) {
  auto doc = PdfiumDocument::open(testdata("text-lines-basic.pdf"));
  ASSERT_TRUE(doc);
  auto lines = doc->text_lines(0);
  ASSERT_TRUE(lines);
  const TextLineInfo* hello = find_line(*lines, "Hello world line");
  ASSERT_NE(hello, nullptr);
  auto covers = doc->line_font_covers(0, hello->id, "Hello there");
  ASSERT_TRUE(covers);
  EXPECT_FALSE(*covers) << "비임베딩 글꼴은 재사용하지 않는다";
  LineReplacement without_fallback{"Hello there", std::nullopt, std::nullopt, std::nullopt};
  auto refused = doc->replace_text_line(0, hello->id, without_fallback);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code, ErrorCode::kInvalidArgument);
  EXPECT_EQ(doc->document_state_count(), 0) << "거절은 토큰을 만들지 않는다";
  const Bitmap before = render(*doc);
  LineReplacement replacement{"Hello there", fallback_for(*hello, "Hello there"), std::nullopt,
                              std::nullopt};
  auto replaced = doc->replace_text_line(0, hello->id, replacement);
  ASSERT_TRUE(replaced) << replaced.error().message;
  EXPECT_TRUE(replaced->used_fallback);
  EXPECT_FALSE(replaced->box.empty());
  ASSERT_EQ(replaced->ids.size(), 1u);
  const std::string text = page_text(*doc);
  EXPECT_NE(text.find("Hello there"), std::string::npos);
  EXPECT_EQ(text.find("Hello world line"), std::string::npos);
  auto objects = doc->text_objects(0);
  ASSERT_TRUE(objects);
  ASSERT_EQ(objects->size(), 1u);
  EXPECT_EQ((*objects)[0].box, replaced->box);
  EXPECT_FALSE((*objects)[0].original_font);
  EXPECT_NEAR((*objects)[0].bbox.y, hello->bbox.y, 6.0) << "같은 자리";
  EXPECT_EQ(diff_outside(before, render(*doc), hello->bbox, 6.0), 0);
  // 새 상자는 OnPdf 텍스트 상자로 다시 고칠 수 있다 (#79: 재수정도 수술 — 토큰이 있다)
  LineReplacement again{"Hello again", fallback_for(*hello, "Hello again"), std::nullopt,
                        std::nullopt};
  auto rewritten = doc->replace_text_line(0, replaced->box, again);
  ASSERT_TRUE(rewritten) << rewritten.error().message;
  EXPECT_NE(rewritten->before, 0u);
  doc->drop_document_state(rewritten->before);
  EXPECT_EQ(rewritten->box, replaced->box);
  EXPECT_NE(page_text(*doc).find("Hello again"), std::string::npos);
}

TEST(TextLines, DisplacedFollowerIsRefused) {
  auto doc = PdfiumDocument::open(testdata("text-lines-basic.pdf"));
  ASSERT_TRUE(doc);
  auto lines = doc->text_lines(0);
  ASSERT_TRUE(lines);
  const TextLineInfo* shifted = find_line(*lines, "Shifted one");
  ASSERT_NE(shifted, nullptr);
  EXPECT_EQ(shifted->object_count, 2);
  const Bitmap before = render(*doc);
  // 줄 전체(두 객체)를 지우는 것은 밀림이 없다
  auto removed = doc->remove_text_line(0, shifted->id);
  ASSERT_TRUE(removed) << removed.error().message;
  ASSERT_TRUE(doc->restore_document_state(removed->before));
  doc->drop_document_state(removed->before);
  EXPECT_EQ(render(*doc).pixels, before.pixels);
}

TEST(TextLines, EncryptedDocumentKeepsPasswordThroughSurgery) {
  auto doc = PdfiumDocument::open(testdata("text-lines-basic-enc.pdf"), "user");
  ASSERT_TRUE(doc) << doc.error().message;
  auto lines = doc->text_lines(0);
  ASSERT_TRUE(lines);
  const TextLineInfo* target = find_line(*lines, "Delete me entirely");
  ASSERT_NE(target, nullptr);
  auto removed = doc->remove_text_line(0, target->id);
  ASSERT_TRUE(removed) << removed.error().message;
  EXPECT_EQ(page_text(*doc).find("Delete me entirely"), std::string::npos);
  ScopedTempDir dir;
  const auto out = dir / "enc.pdf";
  ASSERT_TRUE(save_document_safely(*doc, out, {}));
  EXPECT_FALSE(PdfiumDocument::open(out)) << "여전히 암호가 필요하다";
  auto reopened = PdfiumDocument::open(out, "user");
  ASSERT_TRUE(reopened);
  EXPECT_EQ(page_text(*reopened).find("Delete me entirely"), std::string::npos);
}

TEST(TextLines, SignedDocumentIsRefused) {
  auto doc = PdfiumDocument::open(testdata("signed-dummy.pdf"));
  ASSERT_TRUE(doc);
  auto removed = doc->remove_text_line(0, "p/0/0");
  ASSERT_FALSE(removed);
  EXPECT_EQ(removed.error().code, ErrorCode::kUnsupported);
}

TEST(TextLines, FormXObjectLinesAreEditableOneLevelDeep) {
  auto doc = PdfiumDocument::open(testdata("text-lines-form.pdf"));
  ASSERT_TRUE(doc);
  auto lines = doc->text_lines(0);
  ASSERT_TRUE(lines);
  EXPECT_EQ(lines->size(), 4u) << "2단계 Form 안의 줄은 목록에 없다";
  EXPECT_EQ(find_line(*lines, "Nested form line"), nullptr);
  const TextLineInfo* inside = find_line(*lines, "Inside form first line");
  ASSERT_NE(inside, nullptr);
  EXPECT_TRUE(inside->id.starts_with("f0/"));
  const Bitmap before = render(*doc);
  auto removed = doc->remove_text_line(0, inside->id);
  ASSERT_TRUE(removed) << removed.error().message;
  const std::string text = page_text(*doc);
  EXPECT_EQ(text.find("Inside form first line"), std::string::npos);
  EXPECT_NE(text.find("Inside form second line"), std::string::npos);
  EXPECT_NE(text.find("Nested form line stays"), std::string::npos);
  EXPECT_NE(text.find("Page level line below"), std::string::npos);
  EXPECT_EQ(diff_outside(before, render(*doc), inside->bbox, 5.0), 0) << "이미지·다른 줄 보존";
  auto objects = doc->page_objects(0);
  ASSERT_TRUE(objects);
  EXPECT_EQ(std::count_if(objects->begin(), objects->end(),
                          [](const auto& o) { return o.kind == ObjectKind::kImage; }),
            1);
}

TEST(TextLines, EmbeddedCidLineReusesOriginalFontAndType3FallsBack) {
  auto doc = PdfiumDocument::open(testdata("text-lines-type3.pdf"));
  ASSERT_TRUE(doc);
  auto lines = doc->text_lines(0);
  ASSERT_TRUE(lines);
  EXPECT_EQ(lines->size(), 4u);
  const TextLineInfo* korean = find_line(*lines, "한글 텍스트 줄");
  ASSERT_NE(korean, nullptr);
  EXPECT_TRUE(korean->font_embedded);
  EXPECT_FALSE(korean->font_type3);
  EXPECT_EQ(korean->font_name, "NotoSansKR-subset") << "서브셋 태그를 뗀 이름";
  const TextLineInfo* type3 = find_line(*lines, "ABAB");
  ASSERT_NE(type3, nullptr);
  EXPECT_TRUE(type3->font_type3);
  const TextLineInfo* invisible = find_line(*lines, "Invisible OCR layer text");
  ASSERT_NE(invisible, nullptr);
  EXPECT_FALSE(invisible->editable) << "보이지 않는 글(3 Tr)은 고치면 보이게 된다 → 거절";
  EXPECT_EQ(invisible->reason, "invisible");
  EXPECT_FALSE(doc->remove_text_line(0, invisible->id));
  // 원본 글꼴 커버리지: 서브셋에 있는 글자만
  EXPECT_TRUE(*doc->line_font_covers(0, korean->id, "줄 텍스트 한글"));
  EXPECT_FALSE(*doc->line_font_covers(0, korean->id, "서브셋에 없는 글자 ㉿"));
  EXPECT_FALSE(*doc->line_font_covers(0, type3->id, "AB"));
  const Bitmap before = render(*doc);
  LineReplacement replacement{"줄 텍스트 한글", std::nullopt, std::nullopt, std::nullopt};
  auto replaced = doc->replace_text_line(0, korean->id, replacement);
  ASSERT_TRUE(replaced) << replaced.error().message;
  EXPECT_FALSE(replaced->used_fallback);
  auto objects = doc->text_objects(0);
  ASSERT_TRUE(objects);
  ASSERT_EQ(objects->size(), 1u);
  EXPECT_TRUE((*objects)[0].original_font);
  EXPECT_EQ((*objects)[0].text, "줄 텍스트 한글");
  EXPECT_NEAR((*objects)[0].font_size, 18.0, 0.01);
  const std::string text = page_text(*doc);
  EXPECT_NE(text.find("줄 텍스트 한글"), std::string::npos);
  EXPECT_EQ(text.find("한글 텍스트 줄"), std::string::npos);
  EXPECT_EQ(diff_outside(before, render(*doc), korean->bbox, 7.0), 0);
  // 같은 상자를 다시 원본 글꼴로 고친다 (#79: 재수정도 수술)
  LineReplacement again{"한글 줄", std::nullopt, std::nullopt, std::nullopt};
  auto rewritten = doc->replace_text_line(0, replaced->box, again);
  ASSERT_TRUE(rewritten) << rewritten.error().message;
  EXPECT_NE(rewritten->before, 0u);
  doc->drop_document_state(rewritten->before);
  EXPECT_FALSE(rewritten->used_fallback);
  EXPECT_NE(page_text(*doc).find("한글 줄"), std::string::npos);
  // Type3 줄은 대체 글꼴
  auto lines2 = doc->text_lines(0);
  ASSERT_TRUE(lines2);
  const TextLineInfo* type3_now = find_line(*lines2, "ABAB");
  ASSERT_NE(type3_now, nullptr);
  LineReplacement t3{"BA", fallback_for(*type3_now, "BA"), std::nullopt, std::nullopt};
  auto t3_replaced = doc->replace_text_line(0, type3_now->id, t3);
  ASSERT_TRUE(t3_replaced) << t3_replaced.error().message;
  EXPECT_TRUE(t3_replaced->used_fallback);
  // 저장·왕복
  ScopedTempDir dir;
  const auto out = dir / "type3.pdf";
  auto saved = save_document_safely(*doc, out, {});
  ASSERT_TRUE(saved) << saved.error().message;
  auto reopened = PdfiumDocument::open(out);
  ASSERT_TRUE(reopened);
  const std::string saved_text = page_text(*reopened);
  EXPECT_NE(saved_text.find("한글 줄"), std::string::npos);
  EXPECT_NE(saved_text.find("BA"), std::string::npos);
  EXPECT_EQ(saved_text.find("ABAB"), std::string::npos);
  auto check = QpdfTools::check(out);
  ASSERT_TRUE(check);
  EXPECT_TRUE(check->warnings.empty());
}

TEST(TextLines, DetachedObjectsSurviveReopenAndRestore) {
  auto doc = PdfiumDocument::open(testdata("text-lines-basic.pdf"));
  ASSERT_TRUE(doc);
  ImagePixels px{4, 4, std::vector<std::uint8_t>(64, 0)};
  for (std::size_t k = 0; k < px.bgra.size(); k += 4) px.bgra[k + 3] = 255;
  auto image = doc->add_image_object(0, ImageSpec{px, Rect{300, 100, 40, 40}, {}});
  ASSERT_TRUE(image) << image.error().message;
  auto detached = doc->detach_objects(0, {*image});
  ASSERT_TRUE(detached);
  EXPECT_EQ(doc->detached_count(), 1);
  const Bitmap without_image = render(*doc);
  auto lines = doc->text_lines(0);
  ASSERT_TRUE(lines);
  const TextLineInfo* target = find_line(*lines, "Delete me entirely");
  ASSERT_NE(target, nullptr);
  auto removed = doc->remove_text_line(0, target->id);
  ASSERT_TRUE(removed) << removed.error().message;
  EXPECT_EQ(doc->detached_count(), 1) << "다시 열어도 떼어 둔 객체가 이월된다";
  EXPECT_EQ(diff_outside(without_image, render(*doc), target->bbox, 5.0), 0)
      << "되돌린 이미지가 보이면 안 된다";
  // 되살리기 (반대 순서): 줄 복원 → 이미지 재부착
  ASSERT_TRUE(doc->restore_document_state(removed->before));
  doc->drop_document_state(removed->before);
  EXPECT_EQ(render(*doc).pixels, without_image.pixels);
  EXPECT_EQ(doc->detached_count(), 1);
  ASSERT_TRUE(doc->reattach_objects(*detached));
  auto objects = doc->page_objects(0);
  ASSERT_TRUE(objects);
  EXPECT_EQ(std::count_if(objects->begin(), objects->end(),
                          [&](const auto& o) { return o.id == *image; }),
            1);
  // 버린(drop) 객체는 상태 복원 뒤 되살아나지 않는다
  auto detached2 = doc->detach_objects(0, {*image});
  ASSERT_TRUE(detached2);
  auto lines2 = doc->text_lines(0);
  ASSERT_TRUE(lines2);
  const TextLineInfo* target2 = find_line(*lines2, "Delete me entirely");
  ASSERT_NE(target2, nullptr);
  auto removed2 = doc->remove_text_line(0, target2->id);
  ASSERT_TRUE(removed2) << removed2.error().message;
  doc->drop_detached(*detached2);
  ASSERT_TRUE(doc->restore_document_state(removed2->before));
  doc->drop_document_state(removed2->before);
  auto objects2 = doc->page_objects(0);
  ASSERT_TRUE(objects2);
  EXPECT_EQ(std::count_if(objects2->begin(), objects2->end(),
                          [&](const auto& o) { return o.id == *image; }),
            0);
  EXPECT_EQ(doc->detached_count(), 0);
}

TEST(TextLines, StateTokenLimitEvictsOldest) {
  auto doc = PdfiumDocument::open(testdata("text-lines-basic.pdf"));
  ASSERT_TRUE(doc);
  doc->set_state_byte_limit(1);  // 토큰 하나(방금 것)만 남는다
  auto lines = doc->text_lines(0);
  ASSERT_TRUE(lines);
  const TextLineInfo* first = find_line(*lines, "Delete me entirely");
  ASSERT_NE(first, nullptr);
  auto removed1 = doc->remove_text_line(0, first->id);
  ASSERT_TRUE(removed1) << removed1.error().message;
  auto lines2 = doc->text_lines(0);
  ASSERT_TRUE(lines2);
  const TextLineInfo* second = find_line(*lines2, "Hello world line");
  ASSERT_NE(second, nullptr);
  auto removed2 = doc->remove_text_line(0, second->id);
  ASSERT_TRUE(removed2) << removed2.error().message;
  EXPECT_EQ(doc->document_state_count(), 1);
  auto evicted = doc->restore_document_state(removed1->before);
  ASSERT_FALSE(evicted);
  EXPECT_EQ(evicted.error().code, ErrorCode::kInvalidState);
  ASSERT_TRUE(doc->restore_document_state(removed2->before));
}

// 리뷰 C1: 줄 수정 뒤에 추가해 떼어 둔 객체는 토큰에 없다 → 취소는 성공하되 그 객체는 lost
// (reattach 거절)
TEST(TextLines, DetachedObjectAddedAfterTokenIsLostNotSilentlyEmpty) {
  auto doc = PdfiumDocument::open(testdata("text-lines-basic.pdf"));
  ASSERT_TRUE(doc);
  auto lines = doc->text_lines(0);
  ASSERT_TRUE(lines);
  const TextLineInfo* target = find_line(*lines, "Delete me entirely");
  ASSERT_NE(target, nullptr);
  const Bitmap initial = render(*doc);
  auto removed = doc->remove_text_line(0, target->id);
  ASSERT_TRUE(removed) << removed.error().message;
  ImagePixels px{4, 4, std::vector<std::uint8_t>(64, 0)};
  for (std::size_t k = 0; k < px.bgra.size(); k += 4) px.bgra[k + 3] = 255;
  auto image = doc->add_image_object(0, ImageSpec{px, Rect{300, 100, 40, 40}, {}});
  ASSERT_TRUE(image);
  auto detached = doc->detach_objects(0, {*image});  // AddImage 취소
  ASSERT_TRUE(detached);
  ASSERT_TRUE(doc->restore_document_state(removed->before)) << "줄 삭제 취소는 성공한다";
  EXPECT_EQ(render(*doc).pixels, initial.pixels);
  auto reattached = doc->reattach_objects(*detached);  // AddImage 다시 실행
  ASSERT_FALSE(reattached) << "토큰에 없던 객체는 되살릴 수 없다고 알려야 한다";
  EXPECT_EQ(reattached.error().code, ErrorCode::kInvalidState);
  doc->drop_detached(*detached);
  EXPECT_EQ(doc->detached_count(), 0);
}

// 리뷰 W1: 같은 쪽의 떼어 둔 OnPdf 텍스트 상자(삭제 취소용)가 있어도 줄 수정이 거절되지 않는다
TEST(TextLines, DetachedTextBoxOnSamePageDoesNotBlockEdits) {
  auto doc = PdfiumDocument::open(testdata("text-lines-basic.pdf"));
  ASSERT_TRUE(doc);
  auto lines = doc->text_lines(0);
  ASSERT_TRUE(lines);
  const TextLineInfo* hello = find_line(*lines, "Hello world line");
  ASSERT_NE(hello, nullptr);
  auto added = doc->add_text_objects(0, fallback_for(*hello, "Temporary box"));
  ASSERT_TRUE(added);
  EXPECT_EQ(doc->text_lines(0)->size(), 11u) << "OnPdf 상자는 줄 목록에 들어가지 않는다";
  auto detached = doc->detach_objects(0, added->ids);
  ASSERT_TRUE(detached);
  auto lines2 = doc->text_lines(0);
  const TextLineInfo* target = find_line(*lines2, "Delete me entirely");
  ASSERT_NE(target, nullptr);
  auto removed = doc->remove_text_line(0, target->id);
  ASSERT_TRUE(removed) << removed.error().message;
  EXPECT_EQ(doc->detached_count(), 1);
  ASSERT_TRUE(doc->reattach_objects(*detached));
  EXPECT_NE(page_text(*doc).find("Temporary box"), std::string::npos);
  EXPECT_EQ(page_text(*doc).find("Delete me entirely"), std::string::npos);
}

// 리뷰 I1·I3: TJ 배열, ' 와 " 연산자 줄 — 지우면 줄 이동(T*)은 남아 다음 줄이 제자리
TEST(TextLines, ArrayAndQuoteOperatorsAreRemovedInPlace) {
  auto doc = PdfiumDocument::open(testdata("text-lines-basic.pdf"));
  ASSERT_TRUE(doc);
  const Bitmap before = render(*doc);
  auto lines = doc->text_lines(0);
  ASSERT_TRUE(lines);
  const TextLineInfo* quote = find_line(*lines, "Quote second");
  ASSERT_NE(quote, nullptr);
  auto removed = doc->remove_text_line(0, quote->id);
  ASSERT_TRUE(removed) << removed.error().message;
  const std::string text = page_text(*doc);
  EXPECT_EQ(text.find("Quote second"), std::string::npos);
  EXPECT_NE(text.find("Quote first"), std::string::npos);
  EXPECT_NE(text.find("Double quote third"), std::string::npos);
  EXPECT_EQ(diff_outside(before, render(*doc), quote->bbox, 5.0), 0)
      << "' 를 지워도 뒤 줄(\")은 제자리";
  auto lines2 = doc->text_lines(0);
  ASSERT_TRUE(lines2);
  const TextLineInfo* array = find_line(*lines2, "Array show line");
  ASSERT_NE(array, nullptr);
  const Bitmap mid = render(*doc);
  auto removed2 = doc->remove_text_line(0, array->id);
  ASSERT_TRUE(removed2) << removed2.error().message;
  EXPECT_EQ(page_text(*doc).find("Array show line"), std::string::npos);
  EXPECT_EQ(diff_outside(mid, render(*doc), array->bbox, 5.0), 0);
  // " 연산자 줄: 지워도 자간·어간 설정과 줄 이동은 남는다
  auto lines3 = doc->text_lines(0);
  ASSERT_TRUE(lines3);
  const TextLineInfo* dq = find_line(*lines3, "Double quote third");
  ASSERT_NE(dq, nullptr);
  const Bitmap mid2 = render(*doc);
  auto removed3 = doc->remove_text_line(0, dq->id);
  ASSERT_TRUE(removed3) << removed3.error().message;
  EXPECT_EQ(page_text(*doc).find("Double quote third"), std::string::npos);
  EXPECT_EQ(diff_outside(mid2, render(*doc), dq->bbox, 5.0), 0);
}

// 리뷰 W4: 두 쪽이 같은 Form 을 그린다(직접 /Resources) → 그 안의 줄은 고칠 수 없다
TEST(TextLines, SharedFormXObjectIsRefused) {
  auto doc = PdfiumDocument::open(testdata("text-lines-form.pdf"));
  ASSERT_TRUE(doc);
  auto lines = doc->text_lines(2);
  ASSERT_TRUE(lines);
  const TextLineInfo* shared = find_line(*lines, "Shared form line");
  ASSERT_NE(shared, nullptr);
  EXPECT_TRUE(shared->id.starts_with("f0/"));
  auto removed = doc->remove_text_line(2, shared->id);
  ASSERT_FALSE(removed);
  EXPECT_EQ(removed.error().code, ErrorCode::kUnsupported);
  EXPECT_EQ(removed.error().message, "shared-form");
  EXPECT_EQ(doc->document_state_count(), 0);
  EXPECT_NE(page_text(*doc, 3).find("Shared form line"), std::string::npos) << "4쪽도 그대로";
}

// 리뷰 W3: /Contents 배열이 BT 안에서 나뉘어 앞 스트림의 Tj 를 지우면 뒤 스트림의 Tj 가 밀린다 →
// 거절
TEST(TextLines, DisplacementAcrossContentStreamsIsRefused) {
  auto doc = PdfiumDocument::open(testdata("text-lines-form.pdf"));
  ASSERT_TRUE(doc);
  auto lines = doc->text_lines(1);
  ASSERT_TRUE(lines);
  const TextLineInfo* split = find_line(*lines, "Split one");
  ASSERT_NE(split, nullptr);
  EXPECT_EQ(split->object_count, 2) << "두 스트림의 Tj 가 한 줄로 묶인다";
  // 줄 전체(두 객체)를 지우는 것은 된다
  const Bitmap before = render(*doc, 1);
  auto removed = doc->remove_text_line(1, split->id);
  ASSERT_TRUE(removed) << removed.error().message;
  ASSERT_TRUE(doc->restore_document_state(removed->before));
  doc->drop_document_state(removed->before);
  EXPECT_EQ(render(*doc, 1).pixels, before.pixels);
  // 앞 객체만 지우는 수술은 qpdf 가 밀림으로 거절한다
  ShowOpSurgery ops;
  ops.page_ordinals = {0};
  auto report = QpdfTools::remove_text_show_ops(doc->original_bytes(), "", 1, ops);
  ASSERT_TRUE(report) << report.error().message;
  EXPECT_EQ(report->needs_displacement, 1);
  EXPECT_EQ(report->page_show_ops, 3);
}

// #69: 크기·색 변경 — 원본 글꼴 유지, 기준선 유지(상자 아래쪽이 원래 줄 아래쪽 근처), 줄 밖 보존,
// 복원
TEST(TextLines, FormatChangesSizeAndColorKeepingBaseline) {
  auto doc = PdfiumDocument::open(testdata("text-lines-type3.pdf"));
  ASSERT_TRUE(doc);
  auto lines = doc->text_lines(0);
  ASSERT_TRUE(lines);
  const TextLineInfo* korean = find_line(*lines, "한글 텍스트 줄");
  ASSERT_NE(korean, nullptr);
  const Bitmap before = render(*doc);
  LineReplacement r{"한글 텍스트 줄", std::nullopt, 27.0, Color{0, 0, 200, 255}};
  auto replaced = doc->replace_text_line(0, korean->id, r);
  ASSERT_TRUE(replaced) << replaced.error().message;
  EXPECT_FALSE(replaced->used_fallback);
  auto objects = doc->text_objects(0);
  ASSERT_TRUE(objects);
  ASSERT_EQ(objects->size(), 1u);
  EXPECT_NEAR((*objects)[0].font_size, 27.0, 0.05) << "유효 크기 18 → 27";
  EXPECT_EQ((*objects)[0].color.b, 200);
  EXPECT_NEAR((*objects)[0].baseline_origin.y, korean->bbox.bottom(), 6.0) << "기준선은 그대로";
  EXPECT_EQ(page_text(*doc).find("한글 텍스트 줄") != std::string::npos, true);
  // 기준선 고정이라 줄이 위·오른쪽으로 1.5배 커진다 → 그만큼 넓힌 마스크 밖은 동일
  Rect mask = korean->bbox;
  mask.y -= korean->bbox.h;
  mask.h += korean->bbox.h;
  mask.w = korean->bbox.w * 1.6 + 4;
  EXPECT_EQ(diff_outside(before, render(*doc), mask, 6.0), 0);
  LineReplacement bad{"한글 텍스트 줄", std::nullopt, 0.0, std::nullopt};
  EXPECT_FALSE(doc->replace_text_line(0, replaced->box, bad));
  ASSERT_TRUE(doc->restore_document_state(replaced->before));
  EXPECT_EQ(render(*doc).pixels, before.pixels);
  doc->drop_document_state(replaced->before);
}

// #75: 공백만의 객체(` ( ) Tj`)는 추출 글이 비지만 PDFium 객체·qpdf 표시 연산자 모두에 들어 순번에
// 포함돼야 한다. 빼면 뒤 줄의 순번이 밀려 "content stream does not match page objects" 로 거절됐다
// (코퍼스 65%)
TEST(TextLines, SpaceOnlyObjectsKeepOrdinalsAligned) {
  auto doc = PdfiumDocument::open(testdata("text-lines-basic.pdf"));
  ASSERT_TRUE(doc);
  auto lines = doc->text_lines(0);
  ASSERT_TRUE(lines);
  const TextLineInfo* after = find_line(*lines, "After spaces line");
  ASSERT_NE(after, nullptr);
  EXPECT_TRUE(after->editable);
  const Bitmap before = render(*doc);
  LineReplacement r{"After spaces edited", fallback_for(*after, "After spaces edited"),
                    std::nullopt, std::nullopt};
  auto replaced = doc->replace_text_line(0, after->id, r);
  ASSERT_TRUE(replaced) << replaced.error().message;
  EXPECT_NE(page_text(*doc).find("After spaces edited"), std::string::npos);
  EXPECT_EQ(page_text(*doc).find("After spaces line"), std::string::npos);
  Rect mask = after->bbox;
  mask.w += 40;  // 새 글이 더 길다
  EXPECT_EQ(diff_outside(before, render(*doc), mask, 6.0), 0) << "공백 객체·다른 줄은 그대로";
  // 지우기도 같은 순번 규칙
  auto removed = doc->remove_text_line(0, find_line(*doc->text_lines(0), "Hello world line")->id);
  ASSERT_TRUE(removed) << removed.error().message;
  EXPECT_EQ(page_text(*doc).find("Hello world line"), std::string::npos);
}

// ---- 줄 폭 맞춤 (#77) ----

std::size_t utf8_length(const std::string& s) {
  std::size_t n = 0;
  for (const char ch : s) {
    if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) ++n;
  }
  return n;
}

// 새 상자의 잉크 폭 (쪽 포인트)
double box_width(const PdfiumDocument& doc, const std::string& box) {
  auto objects = doc.text_objects(0);
  EXPECT_TRUE(objects);
  double left = 1e18, right = -1e18;
  for (const auto& o : *objects) {
    if (o.box != box) continue;
    left = std::min(left, o.bbox.x);
    right = std::max(right, o.bbox.right());
  }
  return right > left ? right - left : 0.0;
}

TEST(TextLines, InkWidthAndLetterSpacing) {
  auto doc = PdfiumDocument::open(testdata("text-lines-kern.pdf"));
  ASSERT_TRUE(doc);
  auto lines = doc->text_lines(0);
  ASSERT_TRUE(lines);
  const TextLineInfo* wide = find_line(*lines, "Kerned wide gaps here");
  const TextLineInfo* spaced = find_line(*lines, "Char spaced line here");
  const TextLineInfo* trailing = find_line(*lines, "Trailing spaces line");
  const TextLineInfo* tight = find_line(*lines, "Tightly kerned line");
  const TextLineInfo* plain = find_line(*lines, "Plain line without spacing");
  const TextLineInfo* huge = find_line(*lines, "Huge spacing");
  for (const auto* l : {wide, spaced, trailing, tight, plain, huge}) ASSERT_NE(l, nullptr);
  EXPECT_GT(plain->text_width, 100.0);
  EXPECT_NEAR(plain->letter_spacing, 0.0, 0.1);
  EXPECT_NEAR(spaced->letter_spacing, 2.0, 0.15) << "2 Tc";
  EXPECT_GT(wide->letter_spacing, 0.3)
      << "TJ -500 으로 넓힌 줄 (추출이 넣은 공백 3개의 자연 폭만큼 빠진다)";
  EXPECT_LT(tight->letter_spacing, -0.1) << "TJ 80 으로 좁힌 줄";
  EXPECT_LT(trailing->text_width, trailing->bbox.w - 10.0) << "끝 공백은 잉크 폭에 없다";
  EXPECT_NEAR(trailing->letter_spacing, 0.0, 0.1);
  EXPECT_NEAR(huge->letter_spacing, 20.0, 0.5);
}

// 대체 글꼴(번들 Noto) 경로: 새 폭 ≈ 새 글꼴 자연 폭 + 원 줄 자간 × 간격 수 (글꼴이 달라 원 폭과
// 같지는 않다 — 자간 보존)
double expected_width(const std::string& utf8, double size, double spacing) {
  const auto shaped = bundled_face()->shape(utf8, size);
  const double gaps = static_cast<double>(utf8_length(utf8)) - 1.0;
  return shaped.width + spacing * gaps;
}

TEST(TextLines, ReplaceKeepsWidthWithKerning) {
  auto doc = PdfiumDocument::open(testdata("text-lines-kern.pdf"));
  ASSERT_TRUE(doc);
  const Bitmap before = render(*doc);
  for (const char* needle :
       {"Kerned wide gaps here", "Char spaced line here", "Tightly kerned line"}) {
    auto lines = doc->text_lines(0);
    ASSERT_TRUE(lines);
    const TextLineInfo* line = find_line(*lines, needle);
    ASSERT_NE(line, nullptr) << needle;
    const TextLineInfo info = *line;
    LineReplacement same{info.text, fallback_for(info, info.text), std::nullopt, std::nullopt};
    auto replaced = doc->replace_text_line(0, info.id, same);
    ASSERT_TRUE(replaced) << needle << ": " << replaced.error().message;
    EXPECT_TRUE(replaced->fitted) << needle;
    const double w = box_width(*doc, replaced->box);
    const double expected = expected_width(info.text, info.font_size, info.letter_spacing);
    EXPECT_NEAR(w, expected, std::max(2.5, expected * 0.03))
        << needle << " width " << w << " expected " << expected;
    EXPECT_NE(page_text(*doc).find(info.text), std::string::npos)
        << "자간이 있어도 추출은 공백을 끼우지 않는다: " << page_text(*doc);
    auto objects = doc->text_objects(0);
    ASSERT_TRUE(objects);
    bool found = false;
    for (const auto& o : *objects) {
      if (o.box == replaced->box) {
        EXPECT_NEAR(o.letter_spacing, info.letter_spacing, 0.01) << "마크 ls (부호 포함)";
        found = true;
      }
    }
    EXPECT_TRUE(found);
    ASSERT_TRUE(doc->restore_document_state(replaced->before));
    doc->drop_document_state(replaced->before);
  }
  EXPECT_EQ(render(*doc).pixels, before.pixels);
  // 자간 없는 줄은 커닝을 넣지 않는다
  auto lines = doc->text_lines(0);
  const TextLineInfo plain = *find_line(*lines, "Plain line without spacing");
  EXPECT_NEAR(plain.letter_spacing, 0.0, 0.1);
  LineReplacement same_plain{plain.text, fallback_for(plain, plain.text), std::nullopt,
                             std::nullopt};
  auto replaced_plain = doc->replace_text_line(0, plain.id, same_plain);
  ASSERT_TRUE(replaced_plain) << replaced_plain.error().message;
  EXPECT_FALSE(replaced_plain->fitted);
  ASSERT_TRUE(doc->restore_document_state(replaced_plain->before));
  doc->drop_document_state(replaced_plain->before);
  // 더 긴 글: 글자당 간격이 유지된다 (2 Tc 줄)
  lines = doc->text_lines(0);
  const TextLineInfo spaced = *find_line(*lines, "Char spaced line here");
  const std::string longer = "Char spaced line here and more";
  LineReplacement rep{longer, fallback_for(spaced, longer), std::nullopt, std::nullopt};
  auto replaced = doc->replace_text_line(0, spaced.id, rep);
  ASSERT_TRUE(replaced) << replaced.error().message;
  EXPECT_TRUE(replaced->fitted);
  const double w = box_width(*doc, replaced->box);
  const double expected = expected_width(longer, spaced.font_size, spaced.letter_spacing);
  EXPECT_NEAR(w, expected, std::max(2.5, expected * 0.03));
  ASSERT_TRUE(doc->restore_document_state(replaced->before));
  doc->drop_document_state(replaced->before);
  // 한계: 20 Tc 줄은 |k| ≤ 250 (3pt @12pt) 로 잘린다
  lines = doc->text_lines(0);
  const TextLineInfo huge = *find_line(*lines, "Huge spacing");
  LineReplacement same_huge{huge.text, fallback_for(huge, huge.text), std::nullopt, std::nullopt};
  auto replaced_huge = doc->replace_text_line(0, huge.id, same_huge);
  ASSERT_TRUE(replaced_huge) << replaced_huge.error().message;
  EXPECT_TRUE(replaced_huge->fitted);
  const double hw = box_width(*doc, replaced_huge->box);
  const double capped = expected_width(huge.text, huge.font_size, 3.0);
  EXPECT_NEAR(hw, capped, std::max(2.5, capped * 0.03)) << "3pt 한계";
  EXPECT_LT(hw, huge.text_width * 0.6);
}

TEST(TextLines, SpacingSurvivesSaveAndReopen) {
  auto doc = PdfiumDocument::open(testdata("text-lines-kern.pdf"));
  ASSERT_TRUE(doc);
  auto lines = doc->text_lines(0);
  const TextLineInfo wide = *find_line(*lines, "Kerned wide gaps here");
  LineReplacement same{wide.text, fallback_for(wide, wide.text), std::nullopt, std::nullopt};
  auto replaced = doc->replace_text_line(0, wide.id, same);
  ASSERT_TRUE(replaced) << replaced.error().message;
  const double w0 = box_width(*doc, replaced->box);
  const double expected0 = expected_width(wide.text, wide.font_size, wide.letter_spacing);
  EXPECT_NEAR(w0, expected0, std::max(2.5, expected0 * 0.03));
  ScopedTempDir dir;
  const auto saved = dir / "kern.pdf";
  ASSERT_TRUE(doc->save_to(saved, false));
  auto reopened = PdfiumDocument::open(saved);
  ASSERT_TRUE(reopened);
  EXPECT_NEAR(box_width(*reopened, replaced->box), w0, 0.5) << "저장·다시 열기 뒤 폭 유지";
  // 이동(GenerateContent 재생성)은 Tc 를 쓰지 않아 자연 폭이 되지만, 편집 뒤 정산이 마크 ls 로 다시
  // 넣는다 (#87)
  auto ids = std::vector<std::string>(replaced->ids.begin(), replaced->ids.end());
  ASSERT_TRUE(reopened->move_page_objects(0, ids, {5.0, 7.0}));
  const double natural = expected_width(wide.text, wide.font_size, 0.0);
  EXPECT_NEAR(box_width(*reopened, replaced->box), natural, std::max(2.5, natural * 0.03))
      << "정산 전에는 자연 폭 (PDFium 재생성)";
  const SpacingSettleResult settled = reopened->settle_letter_spacing();
  EXPECT_FALSE(settled.lost);
  EXPECT_EQ(settled.pages_reapplied, 1);
  EXPECT_NEAR(box_width(*reopened, replaced->box), w0, w0 * 0.01) << "정산 뒤 원래 폭 ±1%";
  EXPECT_NE(page_text(*reopened).find(wide.text), std::string::npos) << "추출에 공백이 끼지 않는다";
  const auto saved2 = dir / "kern2.pdf";
  ASSERT_TRUE(reopened->save_to(saved2, false));
  auto again = PdfiumDocument::open(saved2);
  ASSERT_TRUE(again);
  auto objects = again->text_objects(0);
  ASSERT_TRUE(objects);
  ASSERT_EQ(objects->size(), 1u);
  EXPECT_NEAR((*objects)[0].letter_spacing, wide.letter_spacing, 0.01) << "마크 ls 는 남는다";
  EXPECT_NEAR(box_width(*again, replaced->box), w0, w0 * 0.01)
      << "옮긴 뒤 저장·다시 열기에도 폭 유지";
  auto check = QpdfTools::check(saved2);
  ASSERT_TRUE(check);
  EXPECT_TRUE(check->warnings.empty());
}

// qpdf 자간 주입: Tj·TJ 모두 `<cs> Tc … 0 Tc` 로 감싸고, 추출 글은 그대로다 (TJ 커닝이었다면 "C h a
// r" 로 깨진다)
TEST(TextLines, SurgeryFitInjectsCharSpacingAndKeepsExtraction) {
  const auto bytes = onpdf::test::read_bytes(testdata("text-lines-kern.pdf"));
  // 순번: 0 Kerned(TJ) 1 Char spaced(Tj, 2 Tc) 2 Trailing 3 Tightly 4 Plain(Tj) 5 Huge 6 Single
  // array string(TJ)
  ShowOpSurgery ops;
  ops.fits = {{-1, 4, 1.5}, {-1, 6, 1.5}};
  auto report = QpdfTools::remove_text_show_ops(bytes, {}, 0, ops);
  ASSERT_TRUE(report) << report.error().message;
  EXPECT_EQ(report->fitted, 2);
  auto doc = PdfiumDocument::open_from_memory(report->bytes);
  ASSERT_TRUE(doc);
  auto lines = doc->text_lines(0);
  ASSERT_TRUE(lines);
  for (const char* needle : {"Plain line without spacing", "Single array string"}) {
    const TextLineInfo* line = find_line(*lines, needle);
    ASSERT_NE(line, nullptr) << needle << ": " << page_text(*doc);
    EXPECT_NEAR(line->letter_spacing, 1.5, 0.15) << needle;
  }
  EXPECT_NEAR(find_line(*lines, "Kerned wide gaps here")->letter_spacing, 0.4, 0.2)
      << "건드리지 않은 줄";
  EXPECT_NE(page_text(*doc).find("Plain line without spacing"), std::string::npos)
      << "추출은 그대로";
}

// #79: 고친 상자를 다시 고쳐도(수술 경로) 마크 ls 의 자간이 새 글에 적용되고, 취소는 토큰으로 첫
// 상태까지
TEST(TextLines, BoxReeditKeepsLetterSpacing) {
  auto doc = PdfiumDocument::open(testdata("text-lines-kern.pdf"));
  ASSERT_TRUE(doc);
  const Bitmap before = render(*doc);
  auto lines = doc->text_lines(0);
  const TextLineInfo spaced = *find_line(*lines, "Char spaced line here");
  LineReplacement first{spaced.text, fallback_for(spaced, spaced.text), std::nullopt, std::nullopt};
  auto replaced = doc->replace_text_line(0, spaced.id, first);
  ASSERT_TRUE(replaced) << replaced.error().message;
  ASSERT_TRUE(replaced->fitted);
  const std::string longer = "Char spaced again and longer";
  LineReplacement second{longer, fallback_for(spaced, longer), std::nullopt, std::nullopt};
  auto again = doc->replace_text_line(0, replaced->box, second);
  ASSERT_TRUE(again) << again.error().message;
  EXPECT_NE(again->before, 0u);
  EXPECT_TRUE(again->fitted) << "재수정도 자간을 넣는다";
  EXPECT_EQ(again->box, replaced->box);
  const double w = box_width(*doc, again->box);
  const double expected = expected_width(longer, spaced.font_size, spaced.letter_spacing);
  EXPECT_NEAR(w, expected, std::max(2.5, expected * 0.03));
  auto objects = doc->text_objects(0);
  ASSERT_TRUE(objects);
  ASSERT_EQ(objects->size(), 1u) << "옛 상자 객체는 지워졌다";
  EXPECT_NEAR((*objects)[0].letter_spacing, spaced.letter_spacing, 0.01);
  EXPECT_EQ(page_text(*doc).find("Char spaced line here"), std::string::npos);
  ASSERT_TRUE(doc->restore_document_state(again->before));
  doc->drop_document_state(again->before);
  EXPECT_NE(page_text(*doc).find("Char spaced line here"), std::string::npos) << page_text(*doc);
  ASSERT_TRUE(doc->restore_document_state(replaced->before));
  doc->drop_document_state(replaced->before);
  EXPECT_EQ(render(*doc).pixels, before.pixels);
}

// #81: /Contents 배열이 피연산자와 연산자 사이에서 나뉜 쪽(Word·LiveCycle) — 이어 붙여 세고
// 스트림별로 지운다
TEST(TextLines, OperatorSplitAcrossContentStreamsIsEdited) {
  auto doc = PdfiumDocument::open(testdata("text-lines-split.pdf"));
  ASSERT_TRUE(doc);
  const Bitmap before = render(*doc);
  for (const auto& [needle, replacement] : std::vector<std::pair<std::string, std::string>>{
           {"Split operands line", "Joined operands line"}, {"Arr ay split", "Array joined"}}) {
    auto lines = doc->text_lines(0);
    ASSERT_TRUE(lines);
    const TextLineInfo* line = find_line(*lines, needle);
    ASSERT_NE(line, nullptr) << needle;
    EXPECT_TRUE(line->editable);
    const TextLineInfo info = *line;
    LineReplacement r{replacement, fallback_for(info, replacement), std::nullopt, std::nullopt};
    auto replaced = doc->replace_text_line(0, info.id, r);
    ASSERT_TRUE(replaced) << needle << ": " << replaced.error().message;
    EXPECT_NE(page_text(*doc).find(replacement), std::string::npos);
    EXPECT_EQ(page_text(*doc).find(needle), std::string::npos);
    Rect mask = info.bbox;
    mask.w += 30;
    EXPECT_EQ(diff_outside(before, render(*doc), mask, 6.0), 0) << needle;
    ASSERT_TRUE(doc->restore_document_state(replaced->before));
    doc->drop_document_state(replaced->before);
  }
  EXPECT_EQ(render(*doc).pixels, before.pixels);
  // 지우기도 같은 규칙: 나뉜 연산자의 앞 조각(스트림 A 의 피연산자)과 뒤 조각(스트림 B 의 Tj)이
  // 함께 지워진다
  auto lines = doc->text_lines(0);
  const TextLineInfo split = *find_line(*lines, "Split operands line");
  auto removed = doc->remove_text_line(0, split.id);
  ASSERT_TRUE(removed) << removed.error().message;
  EXPECT_EQ(page_text(*doc).find("Split operands line"), std::string::npos);
  EXPECT_NE(page_text(*doc).find("Arr ay split"), std::string::npos) << "뒤 줄은 그대로";
  auto check = QpdfTools::check(testdata("text-lines-split.pdf"));
  ASSERT_TRUE(check);
}

// ---- 자간 재적용 (#87) ----

// 객체 하나의 잉크 폭 (쪽 포인트)
double object_width(const PdfiumDocument& doc, const std::string& id) {
  auto objects = doc.text_objects(0);
  EXPECT_TRUE(objects);
  for (const auto& o : *objects) {
    if (o.id == id) return o.bbox.w;
  }
  ADD_FAILURE() << "object not found: " << id;
  return 0.0;
}

// qpdf 가 유효 Tc 를 추적해 이미 목표 값인 연산자는 건드리지 않는다 (두 번 적용해도 한 번)
TEST(SpacingReapply, SurgeryFitIsIdempotent) {
  const auto bytes = onpdf::test::read_bytes(testdata("text-lines-kern.pdf"));
  // 순번 1 = "Char spaced line here" (원본이 2 Tc) → 목표 2 는 이미 적용됨
  ShowOpSurgery already;
  already.fits = {{-1, 1, 2.0}};
  auto r0 = QpdfTools::remove_text_show_ops(bytes, {}, 0, already);
  ASSERT_TRUE(r0) << r0.error().message;
  EXPECT_EQ(r0->fitted, 0);
  EXPECT_EQ(r0->already_fitted, 1);
  // 순번 4 = Plain (Tc 0): 처음엔 감싸고, 결과에 다시 돌리면 건너뛴다
  ShowOpSurgery ops;
  ops.fits = {{-1, 4, 1.5}};
  auto r1 = QpdfTools::remove_text_show_ops(bytes, {}, 0, ops);
  ASSERT_TRUE(r1) << r1.error().message;
  EXPECT_EQ(r1->fitted, 1);
  auto r2 = QpdfTools::remove_text_show_ops(r1->bytes, {}, 0, ops);
  ASSERT_TRUE(r2) << r2.error().message;
  EXPECT_EQ(r2->fitted, 0);
  EXPECT_EQ(r2->already_fitted, 1);
  // 다른 값이면 다시 감싼다 (바깥 Tc 를 안쪽이 덮는다)
  ShowOpSurgery other;
  other.fits = {{-1, 4, 0.5}};
  auto r3 = QpdfTools::remove_text_show_ops(r1->bytes, {}, 0, other);
  ASSERT_TRUE(r3) << r3.error().message;
  EXPECT_EQ(r3->fitted, 1);
}

// 자간 없는 줄을 고치고 옮겨도 정산은 수술·다시 열기를 하지 않는다
TEST(SpacingReapply, SettleIsNoOpWithoutSpacing) {
  auto doc = PdfiumDocument::open(testdata("text-lines-kern.pdf"));
  ASSERT_TRUE(doc);
  const TextLineInfo plain = *find_line(*doc->text_lines(0), "Plain line without spacing");
  LineReplacement same{plain.text, fallback_for(plain, plain.text), std::nullopt, std::nullopt};
  auto replaced = doc->replace_text_line(0, plain.id, same);
  ASSERT_TRUE(replaced) << replaced.error().message;
  EXPECT_FALSE(replaced->fitted);
  ASSERT_TRUE(doc->move_page_objects(0, replaced->ids, {3.0, 3.0}));
  const SpacingSettleResult settled = doc->settle_letter_spacing();
  EXPECT_FALSE(settled.lost);
  EXPECT_EQ(settled.pages_reapplied, 0);
  EXPECT_EQ(doc->spacing_reapplications(), 0);
  EXPECT_EQ(doc->settle_letter_spacing().pages_reapplied, 0) << "기록이 비면 할 일 없음";
}

// 여러 줄 상자: ls 는 첫 줄에만 있지만 모든 줄에 자간이 들어가고, 같은 스트림의 형제를 지워
// 스트림이 재생성돼도 남은 줄이 되살아난다. 대체 글꼴 상자 추가(ReplaceTextBoxCommand 경로)도
// 처음부터 자간
TEST(SpacingReapply, MultiLineBoxAndSiblingRemoval) {
  auto doc = PdfiumDocument::open(testdata("text-lines-kern.pdf"));
  ASSERT_TRUE(doc);
  const TextLineInfo plain = *find_line(*doc->text_lines(0), "Plain line without spacing");
  TextObjectsSpec spec = fallback_for(plain, "Alpha line one\nBravo line two");
  ASSERT_EQ(spec.lines.size(), 2u);
  spec.letter_spacing = 1.5;
  auto added = doc->add_text_objects(0, spec);
  ASSERT_TRUE(added) << added.error().message;
  ASSERT_EQ(added->ids.size(), 2u);
  const double natural0 = expected_width("Alpha line one", plain.font_size, 0.0);
  const double natural1 = expected_width("Bravo line two", plain.font_size, 0.0);
  EXPECT_NEAR(object_width(*doc, added->ids[0]), natural0, natural0 * 0.03) << "정산 전 자연 폭";
  const SpacingSettleResult settled = doc->settle_letter_spacing();
  EXPECT_FALSE(settled.lost);
  EXPECT_EQ(settled.pages_reapplied, 1);
  const double spaced0 = expected_width("Alpha line one", plain.font_size, 1.5);
  const double spaced1 = expected_width("Bravo line two", plain.font_size, 1.5);
  const double w0 = object_width(*doc, added->ids[0]);
  EXPECT_NEAR(w0, spaced0, std::max(2.0, spaced0 * 0.03)) << "첫 줄";
  EXPECT_NEAR(object_width(*doc, added->ids[1]), spaced1, std::max(2.0, spaced1 * 0.03))
      << "둘째 줄 (마크 ls 는 첫 줄에만)";
  EXPECT_GT(spaced1 - natural1, 10.0) << "픽스처가 자간 차이를 볼 만큼 크다";
  // 형제 삭제 → 같은 스트림 재생성 → 정산
  ASSERT_TRUE(doc->remove_text_objects(0, {added->ids[1]}));
  EXPECT_NEAR(object_width(*doc, added->ids[0]), natural0, natural0 * 0.03)
      << "형제 삭제가 스트림을 다시 써서 자간이 사라진다";
  EXPECT_EQ(doc->settle_letter_spacing().pages_reapplied, 1);
  EXPECT_NEAR(object_width(*doc, added->ids[0]), w0, w0 * 0.01);
  EXPECT_NE(page_text(*doc).find("Alpha line one"), std::string::npos);
  EXPECT_EQ(doc->spacing_reapplications(), 2);
}

// Command 실행 → 취소 → 재실행: 각 단계 뒤 정산(EditController 와 같다)하면 폭이 같다
TEST(SpacingReapply, CommandsKeepWidthThroughUndoRedo) {
  auto doc = PdfiumDocument::open(testdata("text-lines-kern.pdf"));
  ASSERT_TRUE(doc);
  const TextLineInfo wide = *find_line(*doc->text_lines(0), "Kerned wide gaps here");
  LineReplacement same{wide.text, fallback_for(wide, wide.text), std::nullopt, std::nullopt};
  auto replaced = doc->replace_text_line(0, wide.id, same);
  ASSERT_TRUE(replaced) << replaced.error().message;
  ASSERT_TRUE(replaced->fitted);
  const std::string box = replaced->box;
  const double w0 = box_width(*doc, box);
  const auto step = [&](const char* what, Result<ChangeSet> r) {
    ASSERT_TRUE(r) << what << ": " << r.error().message;
    const SpacingSettleResult s = doc->settle_letter_spacing();
    EXPECT_FALSE(s.lost) << what;
    EXPECT_NEAR(box_width(*doc, box), w0, w0 * 0.01) << what;
  };
  edit::MoveObjectsCommand move(0, replaced->ids, {12.0, -4.0});
  step("move", move.execute(*doc));
  step("move undo", move.undo(*doc));
  step("move redo", move.execute(*doc));
  // 대체 글꼴 상자의 색 변경 (삭제 + 재생성) — 이전에는 처음부터 자연 폭이었다
  edit::ReplaceTextBoxCommand recolor(0, box, bundled_face(), std::nullopt, std::nullopt,
                                      Color{200, 20, 20, 255});
  step("recolor", recolor.execute(*doc));
  step("recolor undo", recolor.undo(*doc));
  step("recolor redo", recolor.execute(*doc));
  // 삭제 → 취소(떼어 둔 객체를 되붙임)
  auto objects = doc->text_objects(0);
  ASSERT_TRUE(objects);
  std::vector<std::string> ids;
  for (const auto& o : *objects) {
    if (o.box == box) ids.push_back(o.id);
  }
  edit::RemoveObjectsCommand remove(0, ids);
  ASSERT_TRUE(remove.execute(*doc));
  EXPECT_FALSE(doc->settle_letter_spacing().lost);
  step("remove undo", remove.undo(*doc));
  EXPECT_NE(page_text(*doc).find(wide.text), std::string::npos);
}
}  // namespace
}  // namespace onpdf::engine
