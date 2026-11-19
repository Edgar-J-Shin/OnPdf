// Spike #43: 한글 폰트 임베딩·텍스트 객체 추가 왕복 검증.
// 제품 코드가 아니다 — PDFium 헤더를 engine 밖에서 직접 쓴다(스파이크 예외). 결론만 02-design 으로 옮긴다.
//
// 질문: FPDFText_LoadFont(CID) 로 한글 텍스트 객체를 넣어 저장하면, 시스템 폰트 없이 다시 열어 렌더·추출되고
//       qpdf 검사를 통과하는가? 전체 폰트 임베딩의 파일 크기 증가는 얼마인가?
//
// 입력 폰트: testdata/fonts/NotoSansKR-subset.ttf (저장소, OFL) +
//           ONPDF_SPIKE_FONT_DIR 의 NotoSansKR-Regular-full.ttf / NotoSansKR-spike-subset.ttf (스크래치, 선택)
#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/qpdf_tools.h"
#include "onpdf/core/image/png_io.h"
#include "onpdf/core/platform/path.h"

#include "../test_support.h"

#include <fpdf_edit.h>
#include <fpdf_save.h>
#include <fpdfview.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace onpdf::engine {
namespace {

using onpdf::test::testdata;

std::vector<std::uint8_t> read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

// UTF-8 → UTF-16LE (FPDF_WIDESTRING), BMP + 서로게이트
std::vector<unsigned short> to_utf16(const std::string& utf8) {
  std::vector<unsigned short> out;
  for (std::size_t i = 0; i < utf8.size();) {
    const unsigned char c = static_cast<unsigned char>(utf8[i]);
    char32_t cp = 0;
    int n = 1;
    if (c < 0x80) cp = c;
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
    else { cp = c & 0x07; n = 4; }
    for (int k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + static_cast<std::size_t>(k)]) & 0x3F);
    i += static_cast<std::size_t>(n);
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(static_cast<unsigned short>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<unsigned short>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<unsigned short>(cp));
    }
  }
  out.push_back(0);
  return out;
}

struct FileWriter : FPDF_FILEWRITE {
  std::vector<char> bytes;
  FileWriter() {
    version = 1;
    WriteBlock = [](FPDF_FILEWRITE* self, const void* data, unsigned long size) -> int {
      auto* w = static_cast<FileWriter*>(self);
      const auto* p = static_cast<const char*>(data);
      w->bytes.insert(w->bytes.end(), p, p + size);
      return 1;
    };
  }
};

struct Outcome {
  bool font_loaded = false;
  bool inserted = false;
  bool generated = false;
  std::uintmax_t out_size = 0;
  bool reopened = false;
  bool text_found = false;
  std::string extracted;
  double dark_fraction = 0;
  bool embedded = false;
  std::string base_font;
  bool qpdf_ok = false;
  std::string qpdf_message;
  long load_font_ms = 0;
};

std::filesystem::path spike_out_dir() {
  const char* dir = std::getenv("ONPDF_SPIKE_OUT_DIR");
  return dir ? onpdf::path_from_utf8(dir) : std::filesystem::temp_directory_path();
}

// 입력 문서에 텍스트 객체를 넣어 저장 → 다시 열어 검증
Outcome run_case(const std::string& label, const std::filesystem::path& font_path, int font_type, bool cid,
                 const std::string& text, double x, double y_user) {
  Outcome o;
  const auto font_data = read_file(font_path);
  const auto in_path = testdata("simple.pdf");
  const std::string in_utf8 = onpdf::path_to_utf8(in_path);
  FPDF_DOCUMENT doc = FPDF_LoadDocument(in_utf8.c_str(), nullptr);
  if (doc == nullptr) return o;
  const auto t0 = std::chrono::steady_clock::now();
  FPDF_FONT font = FPDFText_LoadFont(doc, font_data.data(), static_cast<uint32_t>(font_data.size()), font_type, cid ? 1 : 0);
  o.load_font_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  o.font_loaded = font != nullptr;
  if (font != nullptr) {
    FPDF_PAGE page = FPDF_LoadPage(doc, 0);
    FPDF_PAGEOBJECT obj = FPDFPageObj_CreateTextObj(doc, font, 24.0f);
    const auto wide = to_utf16(text);
    if (obj != nullptr && FPDFText_SetText(obj, wide.data())) {
      FPDFPageObj_Transform(obj, 1, 0, 0, 1, x, y_user);
      FPDFPage_InsertObject(page, obj);  // 소유권 이전
      o.inserted = true;
      o.generated = FPDFPage_GenerateContent(page) != 0;
    } else if (obj != nullptr) {
      FPDFPageObj_Destroy(obj);
    }
    FPDF_ClosePage(page);
    FPDFFont_Close(font);
  }
  FileWriter writer;
  if (o.generated) FPDF_SaveAsCopy(doc, &writer, FPDF_NO_INCREMENTAL);
  FPDF_CloseDocument(doc);
  if (writer.bytes.empty()) return o;
  const auto out_path = spike_out_dir() / ("spike-" + label + ".pdf");
  {
    std::ofstream out(out_path, std::ios::binary);
    out.write(writer.bytes.data(), static_cast<std::streamsize>(writer.bytes.size()));
  }
  o.out_size = writer.bytes.size();

  // 다시 열기 (전역 PdfiumLibrary: 시스템 폰트 없음)
  auto reopened = PdfiumDocument::open(out_path);
  o.reopened = reopened.has_value();
  if (!o.reopened) return o;
  auto index = reopened->load_text(0);
  if (index) {
    o.extracted = index->all_text();
    { std::ofstream t(spike_out_dir() / ("spike-" + label + ".txt"), std::ios::binary); t << o.extracted; }
    std::cout << "         chars=" << index->char_count() << "\n";
    o.text_found = o.extracted.find(text) != std::string::npos;
  }
  RenderOptions options;
  options.scale = 2.0f;
  auto bmp = reopened->render_page(0, options);
  if (bmp) {
    (void)image::write_png(*bmp, spike_out_dir() / ("spike-" + label + ".png"));
    // 넣은 자리(사용자 좌표 y_user, 높이 24) 의 어두운 픽셀 비율
    const double page_h = reopened->page_size(0)->height;
    const int x0 = static_cast<int>(x * 2), x1 = std::min(bmp->width, static_cast<int>((x + 300) * 2));
    const int y0 = static_cast<int>((page_h - y_user - 22) * 2), y1 = static_cast<int>((page_h - y_user + 6) * 2);
    int dark = 0, total = 0;
    for (int yy = std::max(0, y0); yy < std::min(bmp->height, y1); ++yy) {
      for (int xx = x0; xx < x1; ++xx) {
        const auto* p = bmp->pixels.data() + static_cast<std::size_t>(yy) * static_cast<std::size_t>(bmp->stride) + static_cast<std::size_t>(xx) * 4;
        ++total;
        if (p[0] < 100 && p[1] < 100 && p[2] < 100) ++dark;
      }
    }
    o.dark_fraction = total ? static_cast<double>(dark) / total : 0.0;
  }
  // 폰트 사전: 임베딩 여부·이름 (raw API 로 다시 열어 확인)
  const std::string out_utf8 = onpdf::path_to_utf8(out_path);
  if (FPDF_DOCUMENT again = FPDF_LoadDocument(out_utf8.c_str(), nullptr)) {
    FPDF_PAGE page = FPDF_LoadPage(again, 0);
    const int n = FPDFPage_CountObjects(page);
    for (int i = n - 1; i >= 0; --i) {
      FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
      if (FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_TEXT) continue;
      FPDF_FONT f = FPDFTextObj_GetFont(obj);
      o.embedded = FPDFFont_GetIsEmbedded(f) == 1;
      char name[128] = {};
      FPDFFont_GetBaseFontName(f, name, sizeof name);
      o.base_font = name;
      break;  // 마지막 텍스트 객체 = 우리가 넣은 것
    }
    FPDF_ClosePage(page);
    FPDF_CloseDocument(again);
  }
  auto check = QpdfTools::check(out_path);
  o.qpdf_ok = check.has_value();
  if (!check) o.qpdf_message = check.error().message;
  else if (!check->warnings.empty()) o.qpdf_message = "warnings: " + std::to_string(check->warnings.size()) + " first: " + check->warnings.front();
  return o;
}

void print(const std::string& label, const Outcome& o, std::uintmax_t font_bytes) {
  std::cout << "[spike] " << label << " font=" << font_bytes << "B load=" << o.load_font_ms << "ms loaded=" << o.font_loaded
            << " inserted=" << o.inserted << " generated=" << o.generated << " out=" << o.out_size << "B reopened=" << o.reopened
            << " text_found=" << o.text_found << " dark=" << o.dark_fraction << " embedded=" << o.embedded << " base=" << o.base_font
            << " qpdf=" << o.qpdf_ok << (o.qpdf_message.empty() ? "" : " (" + o.qpdf_message + ")") << "\n";
  if (!o.text_found) std::cout << "         extracted (" << o.extracted.size() << "B): [" << o.extracted << "]\n";
}

const std::uintmax_t kSimpleSize = std::filesystem::file_size(testdata("simple.pdf"));

// A. 저장소 서브셋 폰트(68KB, 글리프 제한) — CID TrueType
TEST(KoreanTextSpike, RepoSubsetFontCid) {
  const auto font = testdata("fonts") / "NotoSansKR-subset.ttf";
  const std::string text = "한글 문서 작성 테스트 ABC 123";  // 서브셋에 있는 글자만
  auto o = run_case("repo-subset-cid", font, FPDF_FONT_TRUETYPE, true, text, 72, 500);
  print("repo-subset-cid", o, std::filesystem::file_size(font));
  EXPECT_TRUE(o.font_loaded);
  EXPECT_TRUE(o.generated);
  EXPECT_TRUE(o.reopened);
  EXPECT_TRUE(o.text_found) << o.extracted;
  EXPECT_GT(o.dark_fraction, 0.02);
  EXPECT_TRUE(o.embedded);
  EXPECT_TRUE(o.qpdf_ok) << o.qpdf_message;
  std::cout << "[spike] size delta vs simple.pdf (" << kSimpleSize << "B): +" << (o.out_size - kSimpleSize) << "B\n";
}

// A2. 같은 폰트, CID 아님(단순 TrueType) — 한글이 되는지 (예상: 안 됨 → CID 필요 근거)
TEST(KoreanTextSpike, RepoSubsetFontNonCid) {
  const auto font = testdata("fonts") / "NotoSansKR-subset.ttf";
  auto o = run_case("repo-subset-noncid", font, FPDF_FONT_TRUETYPE, false, "한글 문서 작성 테스트 ABC 123", 72, 450);
  print("repo-subset-noncid", o, std::filesystem::file_size(font));
  // 판정은 기록만 (결론에 씀)
}

// B. 전체 정적 폰트(6.2MB) — 임의 문장. ONPDF_SPIKE_FONT_DIR 없으면 건너뜀
TEST(KoreanTextSpike, FullFontCid) {
  const char* dir = std::getenv("ONPDF_SPIKE_FONT_DIR");
  if (dir == nullptr) GTEST_SKIP() << "ONPDF_SPIKE_FONT_DIR not set";
  const auto font = onpdf::path_from_utf8(dir) / "NotoSansKR-Regular-full.ttf";
  if (!std::filesystem::exists(font)) GTEST_SKIP() << "no full font";
  const std::string text = "한글 텍스트 추가 검증 — 임베딩 왕복 2026년 OnPdf 객체 편집 (M3)";
  auto o = run_case("full-cid", font, FPDF_FONT_TRUETYPE, true, text, 72, 400);
  print("full-cid", o, std::filesystem::file_size(font));
  EXPECT_TRUE(o.font_loaded);
  // 발견: 전체 폰트는 공백(U+0020)과 NBSP(U+00A0)가 같은 글리프라 PDFium 자동 ToUnicode 가 U+00A0 을 고른다 → 추출이 어긋난다.
  // 해법은 E(ExplicitCidType2Font): ToUnicode 를 우리가 준다
  EXPECT_FALSE(o.text_found) << "자동 ToUnicode 가 고쳐졌다면 이 기록을 갱신";
  std::string nbsp_fixed = o.extracted;
  for (std::string::size_type at; (at = nbsp_fixed.find("\xC2\xA0")) != std::string::npos;) nbsp_fixed.replace(at, 2, " ");
  EXPECT_NE(nbsp_fixed.find(text), std::string::npos) << "공백 외에는 같아야 한다";
  EXPECT_GT(o.dark_fraction, 0.02);
  EXPECT_TRUE(o.embedded);
  EXPECT_TRUE(o.qpdf_ok) << o.qpdf_message;
  std::cout << "[spike] size delta vs simple.pdf: +" << (o.out_size - kSimpleSize) << "B (전체 임베딩 — PDFium 은 서브셋하지 않는다?)\n";
}

// C. 문장 글자만 남긴 서브셋(13KB, fontTools = HarfBuzz hb-subset 대리) — 같은 문장
TEST(KoreanTextSpike, SentenceSubsetCid) {
  const char* dir = std::getenv("ONPDF_SPIKE_FONT_DIR");
  if (dir == nullptr) GTEST_SKIP() << "ONPDF_SPIKE_FONT_DIR not set";
  const auto font = onpdf::path_from_utf8(dir) / "NotoSansKR-spike-subset.ttf";
  if (!std::filesystem::exists(font)) GTEST_SKIP() << "no subset font";
  const std::string text = "한글 텍스트 추가 검증 — 임베딩 왕복 2026년 OnPdf 객체 편집 (M3)";
  auto o = run_case("sentence-subset-cid", font, FPDF_FONT_TRUETYPE, true, text, 72, 350);
  print("sentence-subset-cid", o, std::filesystem::file_size(font));
  EXPECT_TRUE(o.text_found) << o.extracted;
  EXPECT_GT(o.dark_fraction, 0.02);
  EXPECT_TRUE(o.qpdf_ok) << o.qpdf_message;
  std::cout << "[spike] size delta vs simple.pdf: +" << (o.out_size - kSimpleSize) << "B\n";
}

// D. 두 번 넣으면 폰트가 두 번 임베딩되는가 (같은 FPDF_FONT 재사용 vs LoadFont 두 번)
TEST(KoreanTextSpike, FontReuseAcrossObjects) {
  const auto font_path = testdata("fonts") / "NotoSansKR-subset.ttf";
  const auto data = read_file(font_path);
  const std::string in_utf8 = onpdf::path_to_utf8(testdata("simple.pdf"));
  std::uintmax_t sizes[2] = {0, 0};
  for (int mode = 0; mode < 2; ++mode) {  // 0: 한 폰트 재사용, 1: LoadFont 두 번
    FPDF_DOCUMENT doc = FPDF_LoadDocument(in_utf8.c_str(), nullptr);
    ASSERT_NE(doc, nullptr);
    FPDF_PAGE page = FPDF_LoadPage(doc, 0);
    FPDF_FONT shared = FPDFText_LoadFont(doc, data.data(), static_cast<uint32_t>(data.size()), FPDF_FONT_TRUETYPE, 1);
    ASSERT_NE(shared, nullptr);
    for (int k = 0; k < 2; ++k) {
      FPDF_FONT f = shared;
      if (mode == 1 && k == 1) f = FPDFText_LoadFont(doc, data.data(), static_cast<uint32_t>(data.size()), FPDF_FONT_TRUETYPE, 1);
      FPDF_PAGEOBJECT obj = FPDFPageObj_CreateTextObj(doc, f, 18.0f);
      const auto wide = to_utf16(k == 0 ? "첫째 문장" : "둘째 문장");
      ASSERT_TRUE(FPDFText_SetText(obj, wide.data()));
      FPDFPageObj_Transform(obj, 1, 0, 0, 1, 72, 300 - 30 * k);
      FPDFPage_InsertObject(page, obj);
      if (f != shared) FPDFFont_Close(f);
    }
    ASSERT_TRUE(FPDFPage_GenerateContent(page));
    FileWriter writer;
    FPDF_SaveAsCopy(doc, &writer, FPDF_NO_INCREMENTAL);
    FPDF_ClosePage(page);
    FPDFFont_Close(shared);
    FPDF_CloseDocument(doc);
    sizes[mode] = writer.bytes.size();
  }
  std::cout << "[spike] reuse one font: " << sizes[0] << "B, LoadFont twice: " << sizes[1] << "B\n";
  EXPECT_LT(sizes[0], sizes[1]) << "같은 FPDF_FONT 를 재사용해야 한 번만 임베딩된다";
}

// E. FPDFText_LoadCidType2Font: ToUnicode·CIDToGIDMap 을 우리가 준다 (전체 폰트에서 공백이 U+00A0 로 추출되던 문제의 해법 후보).
//    글자 → GID 는 fontTools 로 미리 계산(제품에서는 FreeType/HarfBuzz 가 한다)
TEST(KoreanTextSpike, ExplicitCidType2Font) {
  const char* dir = std::getenv("ONPDF_SPIKE_FONT_DIR");
  if (dir == nullptr) GTEST_SKIP() << "ONPDF_SPIKE_FONT_DIR not set";
  const std::string text = "한글 텍스트 추가 검증 — 임베딩 왕복 2026년 OnPdf 객체 편집 (M3)";
  for (const char* name : {"NotoSansKR-Regular-full", "NotoSansKR-spike-subset"}) {
    const auto base = onpdf::path_from_utf8(dir) / name;
    const auto font_data = read_file(base.string() + ".ttf");
    const auto cmap = read_file(base.string() + ".tounicode.cmap");
    const auto cid2gid = read_file(base.string() + ".cid2gid.bin");
    std::vector<uint32_t> gids;
    { std::ifstream g(base.string() + ".gids.txt"); uint32_t v; while (g >> v) gids.push_back(v); }
    ASSERT_FALSE(font_data.empty());
    ASSERT_FALSE(gids.empty());
    const std::string cmap_str(cmap.begin(), cmap.end());
    const std::string in_utf8 = onpdf::path_to_utf8(testdata("simple.pdf"));
    FPDF_DOCUMENT doc = FPDF_LoadDocument(in_utf8.c_str(), nullptr);
    ASSERT_NE(doc, nullptr);
    FPDF_FONT font = FPDFText_LoadCidType2Font(doc, font_data.data(), static_cast<uint32_t>(font_data.size()), cmap_str.c_str(),
                                               cid2gid.data(), static_cast<uint32_t>(cid2gid.size()));
    ASSERT_NE(font, nullptr) << name;
    FPDF_PAGE page = FPDF_LoadPage(doc, 0);
    FPDF_PAGEOBJECT obj = FPDFPageObj_CreateTextObj(doc, font, 24.0f);
    ASSERT_TRUE(FPDFText_SetCharcodes(obj, gids.data(), gids.size()));
    FPDFPageObj_Transform(obj, 1, 0, 0, 1, 72, 250);
    FPDFPage_InsertObject(page, obj);
    ASSERT_TRUE(FPDFPage_GenerateContent(page));
    FileWriter writer;
    FPDF_SaveAsCopy(doc, &writer, FPDF_NO_INCREMENTAL);
    FPDF_ClosePage(page);
    FPDFFont_Close(font);
    FPDF_CloseDocument(doc);
    const auto out_path = spike_out_dir() / (std::string("spike-explicit-") + name + ".pdf");
    { std::ofstream out(out_path, std::ios::binary); out.write(writer.bytes.data(), static_cast<std::streamsize>(writer.bytes.size())); }
    auto reopened = PdfiumDocument::open(out_path);
    ASSERT_TRUE(reopened) << name;
    auto index = reopened->load_text(0);
    ASSERT_TRUE(index);
    const std::string extracted = index->all_text();
    EXPECT_NE(extracted.find(text), std::string::npos) << name << ": [" << extracted << "]";
    RenderOptions options;
    options.scale = 2.0f;
    auto bmp = reopened->render_page(0, options);
    ASSERT_TRUE(bmp);
    (void)image::write_png(*bmp, spike_out_dir() / (std::string("spike-explicit-") + name + ".png"));
    auto check = QpdfTools::check(out_path);
    EXPECT_TRUE(check) << (check ? "" : check.error().message);
    std::cout << "[spike] explicit-" << name << " out=" << writer.bytes.size() << "B text_found=" << (extracted.find(text) != std::string::npos)
              << " qpdf=" << check.has_value() << "\n";
  }
}

}  // namespace
}  // namespace onpdf::engine
