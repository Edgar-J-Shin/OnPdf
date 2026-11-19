// PdfiumDocument 이미지·쪽 객체 API (#49): 픽셀(+SMask)·JPEG(DCT) 추가, 목록(기존 이미지 #obj), 마크 부여, 이동·크기, 분리 보관
#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/qpdf_tools.h"
#include "onpdf/core/engine/save.h"
#include "onpdf/core/image/png_io.h"

#include "../test_support.h"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>

namespace onpdf::engine {
namespace {

using onpdf::test::ScopedTempDir;
using onpdf::test::testdata;

// 64×48: 왼쪽 절반 불투명 빨강, 오른쪽 위 1/3 반투명 파랑, 오른쪽 아래 완전 투명
ImagePixels alpha_pixels() {
  ImagePixels p;
  p.width = 64;
  p.height = 48;
  p.bgra.resize(64 * 48 * 4);
  for (int y = 0; y < 48; ++y) {
    for (int x = 0; x < 64; ++x) {
      std::uint8_t* px = p.bgra.data() + (y * 64 + x) * 4;
      if (x < 32) { px[0] = 0; px[1] = 0; px[2] = 255; px[3] = 255; }
      else if (y < 16) { px[0] = 255; px[1] = 0; px[2] = 0; px[3] = 128; }
      else { px[0] = px[1] = px[2] = px[3] = 0; }
    }
  }
  return p;
}

JpegBytes test_jpeg() {
  Bitmap bmp;
  bmp.width = 80;
  bmp.height = 60;
  bmp.stride = 80 * 4;
  bmp.pixels.resize(80 * 60 * 4);
  for (int y = 0; y < 60; ++y)
    for (int x = 0; x < 80; ++x) {
      std::uint8_t* px = bmp.pixels.data() + (y * 80 + x) * 4;
      px[0] = 40; px[1] = 160; px[2] = 40; px[3] = 255;  // 초록
    }
  auto bytes = image::encode_jpeg(bmp);
  EXPECT_TRUE(bytes);
  return JpegBytes{*bytes, 80, 60};
}

double fraction(const Bitmap& b, Rect r, bool (*pred)(int, int, int)) {
  int hit = 0, total = 0;
  for (int y = static_cast<int>(r.y); y < static_cast<int>(r.bottom()); ++y)
    for (int x = static_cast<int>(r.x); x < static_cast<int>(r.right()); ++x) {
      const auto* p = b.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(b.stride) + static_cast<std::size_t>(x) * 4;
      ++total;
      hit += pred(p[2], p[1], p[0]) ? 1 : 0;
    }
  return total ? static_cast<double>(hit) / total : 0;
}

std::string file_bytes(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

TEST(PageObjects, AlphaPixelsShowContentBelowAndJpegStaysDct) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  // simple.pdf 파란 사각형(쪽 포인트 72..272 × 172..292) 위에 덮는다
  ImageSpec png;
  png.source = alpha_pixels();
  png.rect = {72, 162, 200, 150};
  auto png_id = doc->add_image_object(0, png);
  ASSERT_TRUE(png_id) << png_id.error().message;
  ImageSpec jpg;
  jpg.source = test_jpeg();
  jpg.rect = {320, 400, 160, 120};
  auto jpg_id = doc->add_image_object(0, jpg);
  ASSERT_TRUE(jpg_id) << jpg_id.error().message;
  auto list = doc->page_objects(0);
  ASSERT_TRUE(list);
  std::vector<PageObjectInfo> images;
  for (const auto& o : *list) if (o.kind == ObjectKind::kImage) images.push_back(o);
  ASSERT_EQ(images.size(), 2u);
  EXPECT_EQ(images[0].id, *png_id);
  EXPECT_TRUE(images[0].marked);
  EXPECT_EQ(images[0].pixel_width, 64);
  EXPECT_NEAR(images[0].bbox.x, 72, 0.5);
  EXPECT_NEAR(images[0].bbox.y, 162, 0.5);
  EXPECT_NEAR(images[0].bbox.w, 200, 0.5);
  EXPECT_EQ(images[1].filter, "DCTDecode") << "JPEG 원본 그대로";
  EXPECT_EQ(images[1].pixel_width, 80);
  ScopedTempDir dir;
  const auto saved = dir / "images.pdf";
  ASSERT_TRUE(save_document_safely(*doc, saved, {}));
  EXPECT_NE(file_bytes(saved).find("/SMask"), std::string::npos);
  auto reopened = PdfiumDocument::open(saved);
  ASSERT_TRUE(reopened);
  RenderOptions o;
  o.scale = 1.0f;
  auto r = reopened->render_page(0, o);
  ASSERT_TRUE(r);
  // 오른쪽 아래(투명) → 아래 파란 사각형이 보인다, 왼쪽 → 빨강
  EXPECT_GT(fraction(*r, {182, 222, 80, 60}, [](int R, int G, int B) { return B > 200 && R < 60 && G < 60; }), 0.95);
  EXPECT_GT(fraction(*r, {82, 222, 80, 60}, [](int R, int G, int B) { return R > 200 && G < 60 && B < 60; }), 0.95);
  EXPECT_GT(fraction(*r, {330, 410, 140, 100}, [](int R, int G, int B) { return G > 120 && R < 90 && B < 90; }), 0.9);
  EXPECT_TRUE(QpdfTools::check(saved));
  EXPECT_EQ(reopened->page_objects(0)->size(), list->size()) << "마크가 남아 저장 검증 통과";
  std::string png_filter;
  for (const auto& obj : *reopened->page_objects(0)) if (obj.id == *png_id) png_filter = obj.filter;
  EXPECT_EQ(png_filter, "FlateDecode") << "픽셀 이미지는 저장 때 압축된다";
  // 잘못된 입력
  ImageSpec bad = png;
  bad.rect.w = 0;
  EXPECT_FALSE(doc->add_image_object(0, bad));
  ImagePixels mismatch = alpha_pixels();
  mismatch.bgra.pop_back();
  bad = png;
  bad.source = mismatch;
  EXPECT_FALSE(doc->add_image_object(0, bad));
  ImageSpec garbage;
  garbage.source = JpegBytes{{0xFF, 0xD8, 0xFF, 0x00, 1, 2, 3}, 2, 2};  // SOI 뒤가 깨진 JPEG
  EXPECT_FALSE(doc->add_image_object(0, garbage));
  garbage.source = JpegBytes{{1, 2, 3, 4}, 2, 2};
  garbage.rect = {10, 10, 10, 10};
  EXPECT_FALSE(doc->add_image_object(0, garbage));
}

TEST(PageObjects, ExistingImagesListMarkAndKeepRendering) {
  auto doc = PdfiumDocument::open(testdata("scanned.pdf"));
  ASSERT_TRUE(doc);
  RenderOptions o;
  o.scale = 1.0f;
  auto before = doc->render_page(0, o);
  ASSERT_TRUE(before);
  auto list = doc->page_objects(0);
  ASSERT_TRUE(list);
  ASSERT_EQ(list->size(), 1u);
  const PageObjectInfo foreign = list->front();
  EXPECT_EQ(foreign.kind, ObjectKind::kImage);
  EXPECT_FALSE(foreign.marked);
  EXPECT_EQ(foreign.id.rfind("#obj", 0), 0u);
  auto id = doc->mark_object(0, foreign.id);
  ASSERT_TRUE(id) << id.error().message;
  EXPECT_EQ(id->rfind("onpdf-image-", 0), 0u);
  EXPECT_EQ(*doc->mark_object(0, *id), *id) << "이미 마크면 그대로";
  auto after = doc->render_page(0, o);
  ASSERT_TRUE(after);
  int diff = 0;
  for (std::size_t k = 0; k < before->pixels.size(); ++k) diff += before->pixels[k] != after->pixels[k];
  EXPECT_EQ(diff, 0) << "마크 부여 + 콘텐츠 재생성 뒤 렌더 불변";
  auto marked = doc->page_objects(0);
  EXPECT_TRUE(marked->front().marked);
  EXPECT_EQ(marked->front().id, *id);
  EXPECT_FALSE(doc->mark_object(0, "#obj99"));
}

TEST(PageObjects, MoveAndResizeOnAnyRotation) {
  for (int rotation = 0; rotation < 4; ++rotation) {
    auto doc = PdfiumDocument::open(testdata("simple.pdf"));
    ASSERT_TRUE(doc);
    if (rotation) ASSERT_TRUE(doc->set_page_rotation(0, rotation));
    ImageSpec spec;
    spec.source = alpha_pixels();
    spec.rect = {100, 120, 64, 48};
    auto id = doc->add_image_object(0, spec);
    ASSERT_TRUE(id) << "rotation " << rotation;
    auto bbox = [&] {
      for (const auto& o : *doc->page_objects(0)) if (o.id == *id) return o.bbox;
      return Rect{};
    };
    EXPECT_NEAR(bbox().x, 100, 0.5) << "rotation " << rotation;
    EXPECT_NEAR(bbox().y, 120, 0.5) << "rotation " << rotation;
    EXPECT_NEAR(bbox().w, 64, 0.5) << "rotation " << rotation;
    EXPECT_NEAR(bbox().h, 48, 0.5) << "rotation " << rotation;
    ASSERT_TRUE(doc->move_page_objects(0, {*id}, {30, 20}));
    EXPECT_NEAR(bbox().x, 130, 0.5) << "rotation " << rotation;
    EXPECT_NEAR(bbox().y, 140, 0.5) << "rotation " << rotation;
    ASSERT_TRUE(doc->set_object_rect(0, *id, {50, 60, 128, 96}));
    EXPECT_NEAR(bbox().x, 50, 0.5) << "rotation " << rotation;
    EXPECT_NEAR(bbox().y, 60, 0.5) << "rotation " << rotation;
    EXPECT_NEAR(bbox().w, 128, 0.5) << "rotation " << rotation;
    EXPECT_NEAR(bbox().h, 96, 0.5) << "rotation " << rotation;
    EXPECT_FALSE(doc->set_object_rect(0, *id, {0, 0, 0, 10}));
    EXPECT_FALSE(doc->set_object_rect(0, "onpdf-image-ffffffffffffffff", {0, 0, 10, 10}));
  }
}

TEST(PageObjects, DetachReattachKeepsOrderAndRender) {
  auto doc = PdfiumDocument::open(testdata("simple.pdf"));
  ASSERT_TRUE(doc);
  ImageSpec a;
  a.source = alpha_pixels();
  a.rect = {72, 162, 200, 150};
  auto first = doc->add_image_object(0, a);
  ImageSpec b = a;
  b.rect = {150, 200, 100, 75};
  auto second = doc->add_image_object(0, b);
  ASSERT_TRUE(first && second);
  RenderOptions o;
  o.scale = 1.0f;
  auto before = doc->render_page(0, o);
  const auto order_before = *doc->page_objects(0);
  auto detached = doc->detach_objects(0, {*first});
  ASSERT_TRUE(detached);
  EXPECT_EQ(doc->detached_count(), 1);
  EXPECT_EQ(doc->page_objects(0)->size(), order_before.size() - 1);
  ASSERT_TRUE(doc->reattach_objects(*detached));
  EXPECT_EQ(doc->detached_count(), 0);
  const auto order_after = *doc->page_objects(0);
  ASSERT_EQ(order_after.size(), order_before.size());
  for (std::size_t k = 0; k < order_after.size(); ++k) {
    EXPECT_EQ(order_after[k].id, order_before[k].id) << "z 순서 유지";
    EXPECT_EQ(order_after[k].index, order_before[k].index);
  }
  auto after = doc->render_page(0, o);
  int diff = 0;
  for (std::size_t k = 0; k < before->pixels.size(); ++k) diff += before->pixels[k] != after->pixels[k];
  EXPECT_EQ(diff, 0);
  // 여러 개 동시 + drop
  auto both = doc->detach_objects(0, {*first, *second});
  ASSERT_TRUE(both);
  EXPECT_FALSE(doc->reattach_objects(*both + 100));
  doc->drop_detached(*both);
  EXPECT_EQ(doc->detached_count(), 0);
  for (const auto& obj : *doc->page_objects(0)) EXPECT_NE(obj.kind, ObjectKind::kImage);
  EXPECT_FALSE(doc->detach_objects(0, {"onpdf-image-ffffffffffffffff"}));
}

}  // namespace
}  // namespace onpdf::engine
