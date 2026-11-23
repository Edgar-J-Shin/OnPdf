// Spike (#49 설계 프로브): 이미지 객체 — 투명도(SetBitmap 알파 → SMask?), JPEG 원본 삽입, 기존
// 이미지 행렬 변경·마크 부여 뒤 GenerateContent 가 쪽의 다른 내용을 바꾸지 않는가, RemoveObject →
// InsertObjectAtIndex 왕복. 제품 코드 아님.
#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/qpdf_tools.h"
#include "onpdf/core/image/png_io.h"
#include "onpdf/core/platform/path.h"

#include <fpdf_edit.h>
#include <fpdf_save.h>
#include <fpdfview.h>

#include "../test_support.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <string>
#include <vector>

namespace onpdf::engine {
namespace {

using onpdf::test::testdata;

std::filesystem::path probe_dir() {
  const char* dir = std::getenv("ONPDF_SPIKE_IMG_DIR");
  return dir ? onpdf::path_from_utf8(dir) : std::filesystem::path();
}
std::filesystem::path out_dir() {
  const char* dir = std::getenv("ONPDF_SPIKE_OUT_DIR");
  return dir ? onpdf::path_from_utf8(dir) : std::filesystem::temp_directory_path();
}
std::vector<std::uint8_t> read_all(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
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

struct MemoryAccess {
  FPDF_FILEACCESS access{};
  std::vector<std::uint8_t> data;
  explicit MemoryAccess(std::vector<std::uint8_t> bytes) : data(std::move(bytes)) {
    access.m_FileLen = static_cast<unsigned long>(data.size());
    access.m_Param = this;
    access.m_GetBlock = [](void* param, unsigned long pos, unsigned char* buf,
                           unsigned long size) -> int {
      auto* self = static_cast<MemoryAccess*>(param);
      if (pos + size > self->data.size()) return 0;
      std::memcpy(buf, self->data.data() + pos, size);
      return 1;
    };
  }
};

std::string save(FPDF_DOCUMENT doc, const std::string& name) {
  Writer w;
  FPDF_SaveAsCopy(doc, &w, FPDF_NO_INCREMENTAL);
  std::ofstream(out_dir() / name, std::ios::binary) << w.bytes;
  return w.bytes;
}

double fraction(const Bitmap& b, int x0, int y0, int x1, int y1, bool (*pred)(int, int, int)) {
  int hit = 0, total = 0;
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) {
      const auto* p = b.pixels.data() +
                      static_cast<std::size_t>(y) * static_cast<std::size_t>(b.stride) +
                      static_cast<std::size_t>(x) * 4;
      ++total;
      hit += pred(p[2], p[1], p[0]) ? 1 : 0;
    }
  return total ? static_cast<double>(hit) / total : 0;
}

// A. 투명 PNG(BGRA) → SetBitmap → 투명 부분으로 아래 내용(파란 사각형)이 보이는가, 저장에 /SMask 가
// 있는가
TEST(ImageSpike, BgraBitmapAlphaBecomesSmask) {
  if (probe_dir().empty()) GTEST_SKIP();
  auto png = image::read_png(probe_dir() / "alpha.png");  // BGRA
  ASSERT_TRUE(png);
  const std::string in = onpdf::path_to_utf8(testdata("simple.pdf"));
  FPDF_DOCUMENT doc = FPDF_LoadDocument(in.c_str(), nullptr);
  FPDF_PAGE page = FPDF_LoadPage(doc, 0);
  FPDF_BITMAP bmp = FPDFBitmap_CreateEx(png->width, png->height, FPDFBitmap_BGRA,
                                        png->pixels.data(), png->stride);
  ASSERT_NE(bmp, nullptr);
  FPDF_PAGEOBJECT img = FPDFPageObj_NewImageObj(doc);
  ASSERT_TRUE(FPDFImageObj_SetBitmap(&page, 1, img, bmp));
  // simple.pdf 의 파란 사각형(사용자 72..272 × 500..620) 위에 덮는다: 200×150
  const FS_MATRIX m{200, 0, 0, 150, 72, 480};
  ASSERT_TRUE(FPDFPageObj_SetMatrix(img, &m));
  FPDFPage_InsertObject(page, img);
  ASSERT_TRUE(FPDFPage_GenerateContent(page));
  FPDFBitmap_Destroy(bmp);
  const std::string bytes = save(doc, "img-alpha.pdf");
  FPDF_ClosePage(page);
  FPDF_CloseDocument(doc);
  const bool has_smask = bytes.find("/SMask") != std::string::npos;
  std::cout << "[img] /SMask in output: " << has_smask << " size=" << bytes.size() << "\n";
  auto reopened = PdfiumDocument::open(out_dir() / "img-alpha.pdf");
  ASSERT_TRUE(reopened);
  RenderOptions o;
  o.scale = 1.0f;
  auto r = reopened->render_page(0, o);
  ASSERT_TRUE(r);
  (void)image::write_png(*r, out_dir() / "img-alpha.png");
  // 이미지 오른쪽 아래(투명) 영역 = 쪽 포인트 x 180..260, y (792-480-150+150*(16/48))..  → 아래쪽
  // 2/3, 오른쪽 절반
  const int x0 = 72 + 110, x1 = 72 + 190;
  const int y0 = 792 - 480 - 90, y1 = 792 - 500;  // 파란 사각형(쪽 포인트 y 172..292) 안쪽만
  const double blue =
      fraction(*r, x0, y0, x1, y1, [](int R, int G, int B) { return B > 200 && R < 60 && G < 60; });
  const double red = fraction(*r, 72 + 10, y0, 72 + 90, y1,
                              [](int R, int G, int B) { return R > 200 && G < 60 && B < 60; });
  std::cout << "[img] transparent area shows underlying blue: " << blue << ", opaque red: " << red
            << "\n";
  EXPECT_GT(red, 0.9);
  EXPECT_GT(blue, 0.9) << "알파가 SMask 로 들어가 투명 부분에 아래 내용이 보여야 한다";
  EXPECT_TRUE(QpdfTools::check(out_dir() / "img-alpha.pdf"));
}

// B. JPEG 원본 바이트 → LoadJpegFileInline → 저장에 /DCTDecode, 크기 ≈ JPEG
TEST(ImageSpike, JpegInlineKeepsDct) {
  if (probe_dir().empty()) GTEST_SKIP();
  auto jpeg = read_all(probe_dir() / "opaque.jpg");
  ASSERT_FALSE(jpeg.empty());
  const std::string in = onpdf::path_to_utf8(testdata("simple.pdf"));
  FPDF_DOCUMENT doc = FPDF_LoadDocument(in.c_str(), nullptr);
  FPDF_PAGE page = FPDF_LoadPage(doc, 0);
  FPDF_PAGEOBJECT img = FPDFPageObj_NewImageObj(doc);
  MemoryAccess access(jpeg);
  ASSERT_TRUE(FPDFImageObj_LoadJpegFileInline(&page, 1, img, &access.access));
  unsigned int pw = 0, ph = 0;
  ASSERT_TRUE(FPDFImageObj_GetImagePixelSize(img, &pw, &ph));
  std::cout << "[img] jpeg pixel size " << pw << "x" << ph << "\n";
  const FS_MATRIX m{160, 0, 0, 120, 300, 300};
  ASSERT_TRUE(FPDFPageObj_SetMatrix(img, &m));
  FPDFPage_InsertObject(page, img);
  ASSERT_TRUE(FPDFPage_GenerateContent(page));
  const std::string bytes = save(doc, "img-jpeg.pdf");
  FPDF_ClosePage(page);
  FPDF_CloseDocument(doc);
  std::cout << "[img] /DCTDecode: " << (bytes.find("/DCTDecode") != std::string::npos)
            << " size=" << bytes.size() << " (jpeg " << jpeg.size() << ")\n";
  EXPECT_NE(bytes.find("/DCTDecode"), std::string::npos);
  EXPECT_EQ(pw, 80u);
  EXPECT_TRUE(QpdfTools::check(out_dir() / "img-jpeg.pdf"));
}

// C. 기존(다른 프로그램) 이미지: testdata 의 모든 PDF 에서 쪽 직속 이미지 객체에 마크를 붙이고
// 행렬을 1pt 옮겼다 되돌린 뒤
//    GenerateContent → 저장 → 렌더가 원본과 같은가 (콘텐츠 스트림 재작성 위험 측정)
TEST(ImageSpike, ExistingImagesSurviveMarkAndMatrixRoundTrip) {
  int files = 0, images = 0, changed_pages = 0;
  for (const auto& entry : std::filesystem::directory_iterator(testdata(""))) {
    if (entry.path().extension() != ".pdf") continue;
    auto original = PdfiumDocument::open(entry.path());
    if (!original) continue;  // 암호·손상
    const std::string in = onpdf::path_to_utf8(entry.path());
    FPDF_DOCUMENT doc = FPDF_LoadDocument(in.c_str(), nullptr);
    if (doc == nullptr) continue;
    ++files;
    const int pages = std::min(FPDF_GetPageCount(doc), 3);
    std::vector<int> touched_pages;
    for (int p = 0; p < pages; ++p) {
      FPDF_PAGE page = FPDF_LoadPage(doc, p);
      bool touched = false;
      const int n = FPDFPage_CountObjects(page);
      for (int i = 0; i < n; ++i) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
        if (FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_IMAGE) continue;
        ++images;
        FS_MATRIX m{};
        ASSERT_TRUE(FPDFPageObj_GetMatrix(obj, &m));
        FS_MATRIX moved = m;
        moved.e += 1;
        FPDFPageObj_SetMatrix(obj, &moved);
        FPDFPageObj_SetMatrix(obj, &m);
        FPDF_PAGEOBJECTMARK mark = FPDFPageObj_AddMark(obj, "OnPdf");
        FPDFPageObjMark_SetStringParam(doc, obj, mark, "id", "onpdf-image-probe");
        touched = true;
      }
      if (touched) {
        EXPECT_TRUE(FPDFPage_GenerateContent(page)) << entry.path().filename();
        touched_pages.push_back(p);
      }
      FPDF_ClosePage(page);
    }
    if (touched_pages.empty()) {
      FPDF_CloseDocument(doc);
      continue;
    }
    const std::string name = "img-existing-" + entry.path().stem().string() + ".pdf";
    save(doc, name);
    FPDF_CloseDocument(doc);
    auto reopened = PdfiumDocument::open(out_dir() / name);
    ASSERT_TRUE(reopened) << name;
    for (int p : touched_pages) {
      RenderOptions o;
      o.scale = 1.0f;
      auto a = original->render_page(p, o);
      auto b = reopened->render_page(p, o);
      ASSERT_TRUE(a && b);
      int diff = 0;
      for (std::size_t k = 0; k < a->pixels.size(); ++k) diff += a->pixels[k] != b->pixels[k];
      std::cout << "[img] " << entry.path().filename().string() << " p" << p
                << " pixels differ: " << diff << "\n";
      if (diff) ++changed_pages;
    }
    EXPECT_TRUE(QpdfTools::check(out_dir() / name)) << name;
  }
  std::cout << "[img] files=" << files << " images=" << images << " changed_pages=" << changed_pages
            << "\n";
  EXPECT_EQ(changed_pages, 0);
}

// D. RemoveObject → (보관) → InsertObjectAtIndex(원래 자리) → 렌더·z 순서 동일
TEST(ImageSpike, DetachReattachKeepsZOrderAndRender) {
  const std::string in = onpdf::path_to_utf8(testdata("simple.pdf"));
  auto original = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(original);
  RenderOptions o;
  o.scale = 1.0f;
  auto before = original->render_page(0, o);
  FPDF_DOCUMENT doc = FPDF_LoadDocument(in.c_str(), nullptr);
  FPDF_PAGE page = FPDF_LoadPage(doc, 0);
  const int n = FPDFPage_CountObjects(page);
  ASSERT_GT(n, 2);
  const int index = 1;  // 가운데 객체 (z 순서 확인)
  FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, index);
  ASSERT_TRUE(FPDFPage_RemoveObject(page, obj));
  ASSERT_TRUE(FPDFPage_GenerateContent(page));
  EXPECT_EQ(FPDFPage_CountObjects(page), n - 1);
  ASSERT_TRUE(FPDFPage_InsertObjectAtIndex(page, obj, static_cast<size_t>(index)));
  ASSERT_TRUE(FPDFPage_GenerateContent(page));
  EXPECT_EQ(FPDFPage_GetObject(page, index), obj) << "같은 자리";
  save(doc, "img-detach.pdf");
  FPDF_ClosePage(page);
  FPDF_CloseDocument(doc);
  auto reopened = PdfiumDocument::open(out_dir() / "img-detach.pdf");
  ASSERT_TRUE(reopened);
  auto after = reopened->render_page(0, o);
  int diff = 0;
  for (std::size_t k = 0; k < before->pixels.size(); ++k)
    diff += before->pixels[k] != after->pixels[k];
  std::cout << "[img] detach/reattach pixel diff: " << diff << "\n";
  EXPECT_EQ(diff, 0);
}

}  // namespace
}  // namespace onpdf::engine
