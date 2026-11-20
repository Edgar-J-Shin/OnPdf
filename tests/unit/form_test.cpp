// PdfiumDocument 폼 채우기 (#51): 위젯 목록, 텍스트(한글·여러 줄·MaxLen·읽기 전용), 체크·라디오, 콤보·목록,
// 같은 이름 위젯 동기화, 저장·다시 열기, 구조 편집 뒤 유지, 회전 쪽 타일 렌더
#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/qpdf_tools.h"
#include "onpdf/core/engine/save.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iterator>

namespace onpdf::engine {
namespace {

using onpdf::test::ScopedTempDir;
using onpdf::test::testdata;

const FormWidgetInfo* find(const std::vector<FormWidgetInfo>& list, const std::string& name, int control = 0) {
  for (const auto& w : list) if (w.ref.name == name && w.ref.nth == control) return &w;
  return nullptr;
}

FormWidgetInfo widget(PdfiumDocument& doc, int page, const std::string& name, int control = 0) {
  auto list = doc.form_widgets(page);
  EXPECT_TRUE(list);
  const FormWidgetInfo* w = list ? find(*list, name, control) : nullptr;
  EXPECT_NE(w, nullptr) << name;
  return w ? *w : FormWidgetInfo{};
}

TEST(Form, ListsWidgetsWithTypesFlagsAndOptions) {
  auto doc = PdfiumDocument::open(testdata("form-fields.pdf"));
  ASSERT_TRUE(doc);
  EXPECT_EQ(doc->form_type(), FormType::kAcroForm);
  auto list = doc->form_widgets(0);
  ASSERT_TRUE(list);
  EXPECT_EQ(list->size(), 10u);
  const auto* name = find(*list, "name");
  ASSERT_NE(name, nullptr);
  EXPECT_EQ(name->type, FieldType::kText);
  EXPECT_EQ(name->value, "Hong");
  EXPECT_EQ(name->control_count, 2);
  EXPECT_NEAR(name->rect.x, 150, 0.5);
  EXPECT_NEAR(name->rect.y, 792 - 714, 0.5);
  EXPECT_NEAR(name->rect.w, 200, 0.5);
  EXPECT_NEAR(name->font_size, 12.0f, 0.01f);
  EXPECT_TRUE(find(*list, "address")->multiline);
  EXPECT_TRUE(find(*list, "empid")->read_only);
  EXPECT_EQ(find(*list, "empid")->value, "A-1024");
  EXPECT_EQ(find(*list, "zip")->max_len, 5);
  const auto* agree = find(*list, "agree");
  EXPECT_EQ(agree->type, FieldType::kCheckBox);
  EXPECT_FALSE(agree->checked);
  EXPECT_EQ(agree->export_value, "Yes");
  for (int i = 0; i < 3; ++i) {
    const auto* g = find(*list, "gender", i);
    ASSERT_NE(g, nullptr) << i;
    EXPECT_EQ(g->type, FieldType::kRadio);
    EXPECT_FALSE(g->checked) << "기본 선택 없음";
    EXPECT_EQ(g->control_count, 3);
  }
  const auto* city = find(*list, "city");
  EXPECT_EQ(city->type, FieldType::kComboBox);
  EXPECT_TRUE(city->editable_combo);
  EXPECT_EQ(city->options, (std::vector<std::string>{"Seoul", "Busan", "Daegu"}));
  EXPECT_EQ(city->selected, (std::vector<int>{0}));
  const auto* subjects = find(*list, "subjects");
  EXPECT_EQ(subjects->type, FieldType::kListBox);
  EXPECT_TRUE(subjects->multi_select);
  EXPECT_EQ(subjects->options.size(), 4u);
  EXPECT_TRUE(subjects->selected.empty());
  auto second = doc->form_widgets(1);
  ASSERT_TRUE(second);
  ASSERT_NE(find(*second, "name", 0), nullptr) << "같은 필드의 둘째 위젯 (그 쪽에서는 첫째)";
  ASSERT_NE(find(*second, "signature"), nullptr);
  EXPECT_EQ(find(*second, "signature")->type, FieldType::kSignature);
  EXPECT_FALSE(is_fillable(FieldType::kSignature));
}

TEST(Form, KoreanTextSyncsSharedWidgetsAndSurvivesSave) {
  auto doc = PdfiumDocument::open(testdata("form-fields.pdf"));
  ASSERT_TRUE(doc);
  auto pages = doc->set_form_text(0, {"name", 0}, "홍길동");
  ASSERT_TRUE(pages) << pages.error().message;
  EXPECT_EQ(*pages, (std::vector<int>{0, 1})) << "같은 이름 위젯이 있는 쪽 모두";
  EXPECT_EQ(widget(*doc, 0, "name").value, "홍길동");
  EXPECT_EQ(widget(*doc, 1, "name").value, "홍길동");
  ScopedTempDir dir;
  const auto saved = dir / "filled.pdf";
  ASSERT_TRUE(save_document_safely(*doc, saved, {})) << "저장 검증(필드 값 비교) 통과";
  EXPECT_TRUE(QpdfTools::check(saved));
  std::ifstream in(saved, std::ios::binary);
  const std::string bytes{std::istreambuf_iterator<char>(in), {}};
  EXPECT_NE(bytes.find("Korea1"), std::string::npos) << "한글 AP 는 Adobe-Korea1 CID 글꼴 (비임베딩)";
  auto reopened = PdfiumDocument::open(saved);
  ASSERT_TRUE(reopened);
  EXPECT_EQ(widget(*reopened, 0, "name").value, "홍길동");
  EXPECT_EQ(widget(*reopened, 1, "name").value, "홍길동");
  EXPECT_EQ(*reopened->form_values(), *doc->form_values());
}

TEST(Form, MultilineMaxLenReadOnlyAndClear) {
  auto doc = PdfiumDocument::open(testdata("form-fields.pdf"));
  ASSERT_TRUE(doc);
  ASSERT_TRUE(doc->set_form_text(0, {"address", 0}, "1 Main St\n2nd line"));
  const std::string address = widget(*doc, 0, "address").value;
  EXPECT_NE(address.find("1 Main St"), std::string::npos);
  EXPECT_NE(address.find("2nd line"), std::string::npos) << address;
  EXPECT_EQ(address.size(), std::string("1 Main St\n2nd line").size()) << "줄바꿈 한 글자";
  ASSERT_TRUE(doc->set_form_text(0, {"zip", 0}, "1234567"));
  EXPECT_EQ(widget(*doc, 0, "zip").value, "12345") << "MaxLen 5";
  auto denied = doc->set_form_text(0, {"empid", 0}, "X");
  ASSERT_FALSE(denied);
  EXPECT_EQ(denied.error().code, ErrorCode::kPermissionDenied);
  EXPECT_EQ(widget(*doc, 0, "empid").value, "A-1024");
  ASSERT_TRUE(doc->set_form_text(0, {"name", 0}, ""));
  EXPECT_EQ(widget(*doc, 0, "name").value, "") << "빈 값으로 지우기";
  EXPECT_FALSE(doc->set_form_text(0, {"agree", 0}, "x")) << "텍스트 필드가 아님";
  EXPECT_FALSE(doc->set_form_text(0, {"nope", 0}, "x"));
}

TEST(Form, CheckBoxAndRadio) {
  auto doc = PdfiumDocument::open(testdata("form-fields.pdf"));
  ASSERT_TRUE(doc);
  ASSERT_TRUE(doc->set_form_checked(0, {"agree", 0}, true));
  EXPECT_TRUE(widget(*doc, 0, "agree").checked);
  EXPECT_EQ(widget(*doc, 0, "agree").value, "Yes");
  ASSERT_TRUE(doc->set_form_checked(0, {"agree", 0}, true)) << "같은 상태면 그대로";
  ASSERT_TRUE(doc->set_form_checked(0, {"agree", 0}, false));
  EXPECT_FALSE(widget(*doc, 0, "agree").checked);
  ASSERT_TRUE(doc->set_form_checked(0, {"gender", 1}, true));
  EXPECT_EQ(widget(*doc, 0, "gender", 1).value, "F");
  EXPECT_TRUE(widget(*doc, 0, "gender", 1).checked);
  ASSERT_TRUE(doc->set_form_checked(0, {"gender", 0}, true));
  EXPECT_TRUE(widget(*doc, 0, "gender", 0).checked);
  EXPECT_FALSE(widget(*doc, 0, "gender", 1).checked);
  auto cleared = doc->set_form_checked(0, {"gender", 0}, false);
  ASSERT_FALSE(cleared);
  EXPECT_EQ(cleared.error().code, ErrorCode::kUnsupported) << "라디오 해제는 공개 API 로 불가";
  EXPECT_TRUE(widget(*doc, 0, "gender", 0).checked);
  ASSERT_TRUE(doc->set_form_checked(0, {"gender", 1}, false)) << "이미 꺼진 위젯은 그대로";
  ScopedTempDir dir;
  ASSERT_TRUE(save_document_safely(*doc, dir / "checked.pdf", {}));
  auto reopened = PdfiumDocument::open(dir / "checked.pdf");
  ASSERT_TRUE(reopened);
  EXPECT_TRUE(widget(*reopened, 0, "gender", 0).checked);
  EXPECT_FALSE(widget(*reopened, 0, "agree").checked);
}

TEST(Form, ComboAndListSelection) {
  auto doc = PdfiumDocument::open(testdata("form-fields.pdf"));
  ASSERT_TRUE(doc);
  ASSERT_TRUE(doc->set_form_selection(0, {"city", 0}, {2}));
  EXPECT_EQ(widget(*doc, 0, "city").value, "Daegu");
  EXPECT_EQ(widget(*doc, 0, "city").selected, (std::vector<int>{2}));
  ASSERT_TRUE(doc->set_form_text(0, {"city", 0}, "Incheon")) << "편집 가능 콤보는 직접 입력";
  EXPECT_EQ(widget(*doc, 0, "city").value, "Incheon");
  EXPECT_TRUE(widget(*doc, 0, "city").selected.empty());
  EXPECT_FALSE(doc->set_form_selection(0, {"city", 0}, {0, 1})) << "콤보는 하나만";
  EXPECT_FALSE(doc->set_form_selection(0, {"city", 0}, {7}));
  ASSERT_TRUE(doc->set_form_selection(0, {"subjects", 0}, {0, 2}));
  EXPECT_EQ(widget(*doc, 0, "subjects").selected, (std::vector<int>{0, 2}));
  ASSERT_TRUE(doc->set_form_selection(0, {"subjects", 0}, {3}));
  EXPECT_EQ(widget(*doc, 0, "subjects").selected, (std::vector<int>{3}));
  ASSERT_TRUE(doc->set_form_selection(0, {"subjects", 0}, {}));
  EXPECT_TRUE(widget(*doc, 0, "subjects").selected.empty());
  ScopedTempDir dir;
  ASSERT_TRUE(doc->set_form_selection(0, {"subjects", 0}, {1, 3}));
  ASSERT_TRUE(save_document_safely(*doc, dir / "choice.pdf", {}));
  auto reopened = PdfiumDocument::open(dir / "choice.pdf");
  ASSERT_TRUE(reopened);
  EXPECT_EQ(widget(*reopened, 0, "subjects").selected, (std::vector<int>{1, 3}));
  EXPECT_EQ(widget(*reopened, 0, "city").value, "Incheon");
}

TEST(Form, StructuralEditsKeepFieldsWorking) {
  auto doc = PdfiumDocument::open(testdata("form-fields.pdf"));
  ASSERT_TRUE(doc);
  ASSERT_TRUE(doc->set_form_text(0, {"name", 0}, "Before"));  // 필드→쪽 지도를 만든다
  ASSERT_TRUE(doc->move_pages({1}, 0));
  auto first = doc->form_widgets(0);
  ASSERT_TRUE(first);
  ASSERT_NE(find(*first, "name"), nullptr) << "옮긴 2쪽이 앞으로";
  EXPECT_EQ(find(*first, "signature")->type, FieldType::kSignature);
  auto pages = doc->set_form_text(1, {"name", 0}, "After");
  ASSERT_TRUE(pages) << pages.error().message;
  EXPECT_EQ(*pages, (std::vector<int>{0, 1})) << "구조 편집 뒤 지도를 다시 만든다";
  EXPECT_EQ(widget(*doc, 0, "name").value, "After");
}

TEST(Form, DocumentWithoutFormHasNoWidgets) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  EXPECT_EQ(doc->form_type(), FormType::kNone);
  auto list = doc->form_widgets(0);
  ASSERT_TRUE(list);
  EXPECT_TRUE(list->empty());
  auto set = doc->set_form_text(0, {"name", 0}, "x");
  ASSERT_FALSE(set);
  EXPECT_EQ(set.error().code, ErrorCode::kUnsupported);
  EXPECT_TRUE(doc->form_values()->empty());
}

// 회전 쪽: 타일(render_region) 의 위젯이 전체 렌더와 같은 자리에 그려진다
TEST(Form, RotatedPageTileMatchesFullRender) {
  auto doc = PdfiumDocument::open(testdata("form-fields.pdf"));
  ASSERT_TRUE(doc);
  RenderOptions o;
  o.scale = 2.0f;
  auto full = doc->render_page(1, o);
  ASSERT_TRUE(full);
  const Rect region{900, 100, 512, 512};  // 2쪽 이름 칸 (가로 쪽 오른쪽 위)
  auto tile = doc->render_region(1, o, region);
  ASSERT_TRUE(tile);
  int dark_full = 0, big = 0;
  for (int y = 0; y < 512; ++y)
    for (int x = 0; x < 512; ++x)
      for (int c = 0; c < 3; ++c) {
        const auto a = full->pixels[static_cast<std::size_t>((static_cast<int>(region.y) + y) * full->stride + (static_cast<int>(region.x) + x) * 4 + c)];
        const auto b = tile->pixels[static_cast<std::size_t>(y * tile->stride + x * 4 + c)];
        dark_full += a < 128;
        big += std::abs(int(a) - int(b)) > 32;
      }
  EXPECT_GT(dark_full, 100) << "영역에 위젯 테두리·글자가 있다";
  EXPECT_LT(big, 512 * 512 * 3 / 1000) << "타일 위젯이 전체 렌더와 일치";
}

// 쪽 삭제 → 취소(스냅샷에서 되살림) 뒤에도 같은 FieldRef 로 칸을 찾는다. PDFium 은 되살린 위젯을 필드에 다시 붙여
// 컨트롤 index 가 밀리므로 정체성은 쪽 안 순서다 (#51 리뷰 W1)
TEST(Form, FieldRefSurvivesPageDeleteAndRestore) {
  auto doc = PdfiumDocument::open(testdata("form-fields.pdf"));
  ASSERT_TRUE(doc);
  const auto before = *doc->form_widgets(0);
  auto snap = doc->copy_pages_to_snapshot({0});
  ASSERT_TRUE(snap);
  ASSERT_TRUE(doc->delete_pages({0}));
  ASSERT_TRUE(doc->insert_from_snapshot(*snap, {}, 0));
  auto after = doc->form_widgets(0);
  ASSERT_TRUE(after);
  ASSERT_EQ(after->size(), before.size());
  for (std::size_t i = 0; i < before.size(); ++i) {
    EXPECT_EQ((*after)[i].ref, before[i].ref) << before[i].ref.name;
    EXPECT_EQ((*after)[i].value, before[i].value) << before[i].ref.name;
  }
  ASSERT_TRUE(doc->set_form_text(0, {"name", 0}, "Restored"));
  EXPECT_EQ(widget(*doc, 0, "name").value, "Restored");
  EXPECT_EQ(widget(*doc, 1, "name").value, "Restored") << "2쪽 위젯과 값 공유는 그대로";
  ASSERT_TRUE(doc->set_form_checked(0, {"gender", 2}, true));
  EXPECT_TRUE(widget(*doc, 0, "gender", 2).checked);
  RenderOptions o;
  ASSERT_TRUE(doc->render_page(0, o));
}

}  // namespace
}  // namespace onpdf::engine
