// Spike (#51 설계 프로브): 폼 채우기 — ① JS 없이 FORM_* 로 값 설정·확정(텍스트·체크·라디오·콤보) ② 한글 값의 AP 글꼴·렌더
// ③ FPDF_FFLDraw 타일 렌더가 전체 렌더와 맞는가. 제품 코드 아님.
#include "onpdf/core/image/png_io.h"
#include "onpdf/core/platform/path.h"

#include "../test_support.h"

#include <fpdf_annot.h>
#include <fpdf_formfill.h>
#include <fpdf_save.h>
#include <fpdfview.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace onpdf::engine {
namespace {

using onpdf::test::testdata;

std::filesystem::path out_dir() {
  const char* dir = std::getenv("ONPDF_SPIKE_OUT_DIR");
  return dir ? onpdf::path_from_utf8(dir) : std::filesystem::temp_directory_path();
}

std::u16string u16(const char16_t* s) { return s; }

std::string field_string(unsigned long (*fn)(FPDF_FORMHANDLE, FPDF_ANNOTATION, FPDF_WCHAR*, unsigned long), FPDF_FORMHANDLE form,
                         FPDF_ANNOTATION annot) {
  const unsigned long len = fn(form, annot, nullptr, 0);
  if (len <= 2) return {};
  std::u16string buf(len / 2, u'\0');
  fn(form, annot, reinterpret_cast<FPDF_WCHAR*>(buf.data()), len);
  buf.resize(len / 2 - 1);
  std::string out;
  for (char16_t c : buf) {  // BMP 만 (프로브)
    if (c < 0x80) out.push_back(static_cast<char>(c));
    else if (c < 0x800) { out.push_back(static_cast<char>(0xC0 | (c >> 6))); out.push_back(static_cast<char>(0x80 | (c & 0x3F))); }
    else { out.push_back(static_cast<char>(0xE0 | (c >> 12))); out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (c & 0x3F))); }
  }
  return out;
}

struct Writer : FPDF_FILEWRITE {
  std::string bytes;
  Writer() {
    version = 1;
    WriteBlock = [](FPDF_FILEWRITE* self, const void* data, unsigned long size) -> int {
      static_cast<Writer*>(self)->bytes.append(static_cast<const char*>(data), size);
      return 1;
    };
  }
};

struct Form {
  FPDF_FORMFILLINFO info{};
  FPDF_FORMHANDLE handle = nullptr;
  explicit Form(FPDF_DOCUMENT doc) {
    info.version = 1;  // XFA 없음, JS 플랫폼 없음 (m_pJsPlatform = nullptr)
    handle = FPDFDOC_InitFormFillEnvironment(doc, &info);
  }
  ~Form() { if (handle) FPDFDOC_ExitFormFillEnvironment(handle); }
};

FPDF_ANNOTATION find_field(FPDF_PAGE page, FPDF_FORMHANDLE form, const std::string& name, int nth = 0) {
  for (int i = 0; i < FPDFPage_GetAnnotCount(page); ++i) {
    FPDF_ANNOTATION a = FPDFPage_GetAnnot(page, i);
    if (a && field_string(FPDFAnnot_GetFormFieldName, form, a) == name && nth-- == 0) return a;
    if (a) FPDFPage_CloseAnnot(a);
  }
  return nullptr;
}

Bitmap to_bitmap(FPDF_BITMAP bmp) {
  Bitmap out;
  out.width = FPDFBitmap_GetWidth(bmp);
  out.height = FPDFBitmap_GetHeight(bmp);
  out.stride = FPDFBitmap_GetStride(bmp);
  const auto* p = static_cast<const std::uint8_t*>(FPDFBitmap_GetBuffer(bmp));
  out.pixels.assign(p, p + static_cast<std::size_t>(out.stride) * out.height);
  return out;
}

// 전체 렌더: 쪽 + 위젯
FPDF_BITMAP render_full(FPDF_PAGE page, FPDF_FORMHANDLE form, double scale, int flags) {
  const int w = static_cast<int>(FPDF_GetPageWidthF(page) * scale), h = static_cast<int>(FPDF_GetPageHeightF(page) * scale);
  FPDF_BITMAP bmp = FPDFBitmap_Create(w, h, 1);
  FPDFBitmap_FillRect(bmp, 0, 0, w, h, 0xFFFFFFFF);
  FPDF_RenderPageBitmap(bmp, page, 0, 0, w, h, 0, flags);
  if (form) FPDF_FFLDraw(form, bmp, page, 0, 0, w, h, 0, flags);
  return bmp;
}

int dark_pixels(FPDF_BITMAP bmp, int x0, int y0, int x1, int y1) {
  const auto* p = static_cast<const std::uint8_t*>(FPDFBitmap_GetBuffer(bmp));
  const int stride = FPDFBitmap_GetStride(bmp);
  int n = 0;
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) {
      const auto* px = p + y * stride + x * 4;
      n += (px[0] < 100 && px[1] < 100 && px[2] < 100) ? 1 : 0;
    }
  return n;
}

TEST(FormSpike, SetValuesWithoutJavaScriptAndSave) {
  FPDF_DOCUMENT doc = FPDF_LoadDocument(onpdf::path_to_utf8(testdata("form-acro.pdf")).c_str(), nullptr);
  ASSERT_NE(doc, nullptr);
  std::cout << "form type " << FPDF_GetFormType(doc) << "\n";
  {
    Form form(doc);
    ASSERT_NE(form.handle, nullptr);
    FPDF_SetFormFieldHighlightAlpha(form.handle, 0);  // 프로브: 강조 없음
    FPDF_PAGE page = FPDF_LoadPage(doc, 0);
    FORM_OnAfterLoadPage(page, form.handle);
    // 렌더 전: 위젯이 FFLDraw 없이 그려지는가
    {
      FPDF_BITMAP plain = render_full(page, nullptr, 1.0, FPDF_ANNOT);
      FPDF_BITMAP with = render_full(page, form.handle, 1.0, FPDF_ANNOT);
      std::cout << "name field dark px: without FFLDraw " << dark_pixels(plain, 150, 88, 350, 112) << ", with "
                << dark_pixels(with, 150, 88, 350, 112) << "\n";
      FPDFBitmap_Destroy(plain);
      FPDFBitmap_Destroy(with);
    }
    // ① 텍스트: 포커스 → 전체 선택 → 바꾸기 → 포커스 해제
    FPDF_ANNOTATION name = find_field(page, form.handle, "name");
    ASSERT_NE(name, nullptr);
    std::cout << "name type " << FPDFAnnot_GetFormFieldType(form.handle, name) << " value '"
              << field_string(FPDFAnnot_GetFormFieldValue, form.handle, name) << "'\n";
    unsigned long ap_before = FPDFAnnot_GetAP(name, FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr, 0);
    ASSERT_TRUE(FORM_SetFocusedAnnot(form.handle, name));
    FORM_SelectAllText(form.handle, page);
    const std::u16string text = u"홍길동 Kim";
    FORM_ReplaceSelection(form.handle, page, reinterpret_cast<FPDF_WIDESTRING>(text.c_str()));
    ASSERT_TRUE(FORM_ForceToKillFocus(form.handle));
    const std::string value = field_string(FPDFAnnot_GetFormFieldValue, form.handle, name);
    std::cout << "name after '" << value << "' AP " << ap_before << " -> "
              << FPDFAnnot_GetAP(name, FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr, 0) << "\n";
    EXPECT_EQ(value, "홍길동 Kim");
    FPDFPage_CloseAnnot(name);
    // 체크박스: 가운데 클릭
    FPDF_ANNOTATION agree = find_field(page, form.handle, "agree");
    ASSERT_NE(agree, nullptr);
    const bool was = FPDFAnnot_IsChecked(form.handle, agree);
    FORM_OnLButtonDown(form.handle, page, 0, 159, 649);
    FORM_OnLButtonUp(form.handle, page, 0, 159, 649);
    FORM_ForceToKillFocus(form.handle);
    std::cout << "agree " << was << " -> " << FPDFAnnot_IsChecked(form.handle, agree) << "\n";
    EXPECT_NE(was, static_cast<bool>(FPDFAnnot_IsChecked(form.handle, agree)));
    FPDFPage_CloseAnnot(agree);
    // 체크박스 대안: 포커스 + 스페이스
    agree = find_field(page, form.handle, "agree");
    ASSERT_TRUE(FORM_SetFocusedAnnot(form.handle, agree));
    FORM_OnChar(form.handle, page, ' ', 0);
    FORM_ForceToKillFocus(form.handle);
    std::cout << "agree after space " << FPDFAnnot_IsChecked(form.handle, agree) << "\n";
    FPDFPage_CloseAnnot(agree);
    // 라디오: 둘째 위젯 클릭
    FORM_OnLButtonDown(form.handle, page, 0, 258, 608);
    FORM_OnLButtonUp(form.handle, page, 0, 258, 608);
    FORM_ForceToKillFocus(form.handle);
    FPDF_ANNOTATION r1 = find_field(page, form.handle, "choice", 0);
    FPDF_ANNOTATION r2 = find_field(page, form.handle, "choice", 1);
    ASSERT_TRUE(r1 && r2);
    std::cout << "radio value '" << field_string(FPDFAnnot_GetFormFieldValue, form.handle, r2) << "' r1 "
              << FPDFAnnot_IsChecked(form.handle, r1) << " r2 " << FPDFAnnot_IsChecked(form.handle, r2) << " idx2 "
              << FPDFAnnot_GetFormControlIndex(form.handle, r2) << " count " << FPDFAnnot_GetFormControlCount(form.handle, r2)
              << " export '" << field_string(FPDFAnnot_GetFormFieldExportValue, form.handle, r2) << "'\n";
    EXPECT_TRUE(FPDFAnnot_IsChecked(form.handle, r2));
    EXPECT_FALSE(FPDFAnnot_IsChecked(form.handle, r1));
    FPDFPage_CloseAnnot(r1);
    FPDFPage_CloseAnnot(r2);
    // 콤보: 포커스 + SetIndexSelected
    FPDF_ANNOTATION city = find_field(page, form.handle, "city");
    ASSERT_NE(city, nullptr);
    std::cout << "city options " << FPDFAnnot_GetOptionCount(form.handle, city) << "\n";
    ASSERT_TRUE(FORM_SetFocusedAnnot(form.handle, city));
    const bool selected = FORM_SetIndexSelected(form.handle, page, 1, true);
    FORM_ForceToKillFocus(form.handle);
    std::cout << "city set " << selected << " value '" << field_string(FPDFAnnot_GetFormFieldValue, form.handle, city) << "'\n";
    EXPECT_EQ(field_string(FPDFAnnot_GetFormFieldValue, form.handle, city), "Busan");
    // 콤보 대안: 편집 불가 콤보에 ReplaceSelection
    ASSERT_TRUE(FORM_SetFocusedAnnot(form.handle, city));
    FORM_SelectAllText(form.handle, page);
    const std::u16string daegu = u"Daegu";
    FORM_ReplaceSelection(form.handle, page, reinterpret_cast<FPDF_WIDESTRING>(daegu.c_str()));
    FORM_ForceToKillFocus(form.handle);
    std::cout << "city after replace '" << field_string(FPDFAnnot_GetFormFieldValue, form.handle, city) << "'\n";
    FPDFPage_CloseAnnot(city);
    // 렌더 (시스템 글꼴 꺼짐) → 한글 칸 픽셀
    FPDF_BITMAP filled = render_full(page, form.handle, 2.0, FPDF_ANNOT);
    std::cout << "filled name dark px (scale 2) " << dark_pixels(filled, 300, 176, 700, 224) << "\n";
    ASSERT_TRUE(image::write_png(to_bitmap(filled), out_dir() / "form-filled-live.png"));
    FPDFBitmap_Destroy(filled);
    FORM_OnBeforeClosePage(page, form.handle);
    FPDF_ClosePage(page);
    Writer writer;
    ASSERT_TRUE(FPDF_SaveAsCopy(doc, &writer, 0));
    std::ofstream(out_dir() / "form-filled.pdf", std::ios::binary) << writer.bytes;
    std::cout << "saved " << writer.bytes.size() << " bytes → " << (out_dir() / "form-filled.pdf") << "\n";
  }
  FPDF_CloseDocument(doc);
  // 다시 열기: 값·렌더
  FPDF_DOCUMENT again = FPDF_LoadDocument(onpdf::path_to_utf8(out_dir() / "form-filled.pdf").c_str(), nullptr);
  ASSERT_NE(again, nullptr);
  {
    Form form(again);
    FPDF_PAGE page = FPDF_LoadPage(again, 0);
    FORM_OnAfterLoadPage(page, form.handle);
    FPDF_ANNOTATION name = find_field(page, form.handle, "name");
    std::cout << "reopened name '" << field_string(FPDFAnnot_GetFormFieldValue, form.handle, name) << "'\n";
    FPDFPage_CloseAnnot(name);
    FPDF_BITMAP plain = render_full(page, nullptr, 2.0, FPDF_ANNOT);
    std::cout << "reopened name dark px without form env " << dark_pixels(plain, 300, 176, 700, 224) << "\n";
    ASSERT_TRUE(image::write_png(to_bitmap(plain), out_dir() / "form-filled-reopened-noenv.png"));
    FPDFBitmap_Destroy(plain);
    FORM_OnBeforeClosePage(page, form.handle);
    FPDF_ClosePage(page);
  }
  FPDF_CloseDocument(again);
}

// ③ 타일: RenderPageBitmapWithMatrix(평행 이동) + FFLDraw(음수 시작점) == 전체 렌더의 같은 영역
TEST(FormSpike, FflDrawTileMatchesFullRender) {
  FPDF_DOCUMENT doc = FPDF_LoadDocument(onpdf::path_to_utf8(testdata("form-acro.pdf")).c_str(), nullptr);
  ASSERT_NE(doc, nullptr);
  {
    Form form(doc);
    FPDF_PAGE page = FPDF_LoadPage(doc, 0);
    FORM_OnAfterLoadPage(page, form.handle);
    const double scale = 2.0;
    const int W = static_cast<int>(612 * scale), H = static_cast<int>(792 * scale);
    FPDF_BITMAP full = render_full(page, form.handle, scale, FPDF_ANNOT);
    const int x0 = 256, y0 = 128, T = 512;  // 이름·체크 칸을 가르는 타일
    FPDF_BITMAP tile = FPDFBitmap_Create(T, T, 1);
    FPDFBitmap_FillRect(tile, 0, 0, T, T, 0xFFFFFFFF);
    FS_MATRIX m{static_cast<float>(scale), 0, 0, static_cast<float>(scale), static_cast<float>(-x0), static_cast<float>(-y0)};
    FS_RECTF clip{0, 0, static_cast<float>(T), static_cast<float>(T)};
    FPDF_RenderPageBitmapWithMatrix(tile, page, &m, &clip, FPDF_ANNOT);
    FPDF_FFLDraw(form.handle, tile, page, -x0, -y0, W, H, 0, FPDF_ANNOT);
    const auto* fp = static_cast<const std::uint8_t*>(FPDFBitmap_GetBuffer(full));
    const auto* tp = static_cast<const std::uint8_t*>(FPDFBitmap_GetBuffer(tile));
    const int fs = FPDFBitmap_GetStride(full), ts = FPDFBitmap_GetStride(tile);
    int diff = 0, big = 0;
    for (int y = 0; y < T; ++y)
      for (int x = 0; x < T; ++x)
        for (int c = 0; c < 3; ++c) {
          const int d = std::abs(fp[(y0 + y) * fs + (x0 + x) * 4 + c] - tp[y * ts + x * 4 + c]);
          diff += d > 0;
          big += d > 32;
        }
    std::cout << "tile vs full: differing channels " << diff << " (>32: " << big << ") of " << T * T * 3 << "\n";
    ASSERT_TRUE(image::write_png(to_bitmap(tile), out_dir() / "form-tile.png"));
    EXPECT_LT(big, T * T * 3 / 1000);
    FPDFBitmap_Destroy(tile);
    FPDFBitmap_Destroy(full);
    FORM_OnBeforeClosePage(page, form.handle);
    FPDF_ClosePage(page);
  }
  FPDF_CloseDocument(doc);
}

// 라디오·체크를 "선택 없음"으로 되돌릴 수 있는가 (실행 취소), 공용 API 로 읽는 /AS·/V
TEST(FormSpike, RadioBackToOff) {
  FPDF_DOCUMENT doc = FPDF_LoadDocument(onpdf::path_to_utf8(testdata("form-acro.pdf")).c_str(), nullptr);
  ASSERT_NE(doc, nullptr);
  {
    Form form(doc);
    FPDF_PAGE page = FPDF_LoadPage(doc, 0);
    FORM_OnAfterLoadPage(page, form.handle);
    auto state = [&] {
      FPDF_ANNOTATION r1 = find_field(page, form.handle, "choice", 0);
      FPDF_ANNOTATION r2 = find_field(page, form.handle, "choice", 1);
      std::string s = "r1=" + std::to_string(FPDFAnnot_IsChecked(form.handle, r1)) + " r2=" +
                      std::to_string(FPDFAnnot_IsChecked(form.handle, r2)) + " V='" +
                      field_string(FPDFAnnot_GetFormFieldValue, form.handle, r1) + "' flags=" +
                      std::to_string(FPDFAnnot_GetFormFieldFlags(form.handle, r1));
      FPDFPage_CloseAnnot(r1);
      FPDFPage_CloseAnnot(r2);
      return s;
    };
    std::cout << "start " << state() << "\n";
    FORM_OnLButtonDown(form.handle, page, 0, 158, 608);  // 이미 선택된 r1 다시 클릭 (NoToggleToOff)
    FORM_OnLButtonUp(form.handle, page, 0, 158, 608);
    FORM_ForceToKillFocus(form.handle);
    std::cout << "click checked r1 " << state() << "\n";
    FPDF_ANNOTATION r1 = find_field(page, form.handle, "choice", 0);
    FORM_SetFocusedAnnot(form.handle, r1);
    FORM_OnChar(form.handle, page, ' ', 0);
    FORM_ForceToKillFocus(form.handle);
    FPDFPage_CloseAnnot(r1);
    std::cout << "space on r1 " << state() << "\n";
    // NoToggleToOff(비트 15) 를 잠시 끄고 클릭 → 플래그 복원
    r1 = find_field(page, form.handle, "choice", 0);
    const int flags = FPDFAnnot_GetFormFieldFlags(form.handle, r1);
    std::cout << "set flags " << FPDFAnnot_SetFormFieldFlags(form.handle, r1, flags & ~(1 << 14)) << "\n";
    FPDFPage_CloseAnnot(r1);
    FORM_OnLButtonDown(form.handle, page, 0, 158, 608);
    FORM_OnLButtonUp(form.handle, page, 0, 158, 608);
    FORM_ForceToKillFocus(form.handle);
    r1 = find_field(page, form.handle, "choice", 0);
    FPDFAnnot_SetFormFieldFlags(form.handle, r1, flags);
    FPDFPage_CloseAnnot(r1);
    std::cout << "toggle off via flag " << state() << "\n";
    {
      Writer writer;
      ASSERT_TRUE(FPDF_SaveAsCopy(doc, &writer, 0));
      const auto at = writer.bytes.find("/T(choice)");
      std::cout << "saved choice dict: " << writer.bytes.substr(at == std::string::npos ? 0 : (at > 80 ? at - 80 : 0), 200) << "\n";
    }
    // 체크박스 끄기: 클릭 (토글)
    FPDF_ANNOTATION agree = find_field(page, form.handle, "agree");
    std::cout << "agree flags " << FPDFAnnot_GetFormFieldFlags(form.handle, agree) << " export '"
              << field_string(FPDFAnnot_GetFormFieldExportValue, form.handle, agree) << "'\n";
    FPDFPage_CloseAnnot(agree);
    FORM_OnBeforeClosePage(page, form.handle);
    FPDF_ClosePage(page);
  }
  FPDF_CloseDocument(doc);
}

// 플래그를 바꾸고 폼 환경을 다시 만들면 NoToggleToOff 가 풀리는가 → 라디오 선택 없음으로 되돌리기
TEST(FormSpike, RadioOffViaEnvironmentReload) {
  FPDF_DOCUMENT doc = FPDF_LoadDocument(onpdf::path_to_utf8(testdata("form-acro.pdf")).c_str(), nullptr);
  ASSERT_NE(doc, nullptr);
  int flags = 0;
  {
    Form form(doc);
    FPDF_PAGE page = FPDF_LoadPage(doc, 0);
    FORM_OnAfterLoadPage(page, form.handle);
    FPDF_ANNOTATION r1 = find_field(page, form.handle, "choice", 0);
    flags = FPDFAnnot_GetFormFieldFlags(form.handle, r1);
    ASSERT_TRUE(FPDFAnnot_SetFormFieldFlags(form.handle, r1, flags & ~(1 << 14)));
    FPDFPage_CloseAnnot(r1);
    FORM_OnBeforeClosePage(page, form.handle);
    FPDF_ClosePage(page);
  }
  {
    Form form(doc);  // 다시 만든 환경: 필드가 새 플래그로 읽힌다
    FPDF_PAGE page = FPDF_LoadPage(doc, 0);
    FORM_OnAfterLoadPage(page, form.handle);
    FORM_OnLButtonDown(form.handle, page, 0, 158, 608);
    FORM_OnLButtonUp(form.handle, page, 0, 158, 608);
    FORM_ForceToKillFocus(form.handle);
    FPDF_ANNOTATION r1 = find_field(page, form.handle, "choice", 0);
    FPDF_ANNOTATION r2 = find_field(page, form.handle, "choice", 1);
    std::cout << "after reload click r1=" << FPDFAnnot_IsChecked(form.handle, r1) << " r2=" << FPDFAnnot_IsChecked(form.handle, r2)
              << " V='" << field_string(FPDFAnnot_GetFormFieldValue, form.handle, r1) << "'\n";
    ASSERT_TRUE(FPDFAnnot_SetFormFieldFlags(form.handle, r1, flags));
    FPDFPage_CloseAnnot(r1);
    FPDFPage_CloseAnnot(r2);
    FORM_OnBeforeClosePage(page, form.handle);
    FPDF_ClosePage(page);
    Writer writer;
    ASSERT_TRUE(FPDF_SaveAsCopy(doc, &writer, 0));
    std::ofstream(out_dir() / "form-radio-off.pdf", std::ios::binary) << writer.bytes;
  }
  FPDF_CloseDocument(doc);
}

}  // namespace
}  // namespace onpdf::engine
