// Spike #55 후속: M3 객체 편집(#47·#49)이 기존 문서에서 PDFium 콘텐츠 재생성 손실을 내는가.
// 제품 엔진(PdfiumDocument)을 그대로 써서 시나리오별로 편집 → 저장 → 다시 열기 → 편집 영역 밖 픽셀 비교.
//
// 실행: onpdf_spike_m3_regen <out-dir> a.pdf b.pdf ...   (쪽 0, 1)
// 시나리오
//   add   새 이미지 추가(쪽 모서리 10×10pt) — 새 객체만이라 원본 스트림은 그대로여야 한다
//   move  기존(마크 없는) 이미지를 1pt 이동 — mark_object + move_page_objects
//   del   기존 이미지 삭제 — detach_objects
#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/pdfium_library.h"
#include "onpdf/core/platform/path.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace onpdf;
using namespace onpdf::engine;

namespace {

std::vector<std::uint8_t> read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

constexpr double kScale = 2.0;

struct Stat {
  long outside_big = -1;
  std::string error;
};

// 쪽 포인트 사각형들(여유 2pt) 밖에서 채널 차이 > 48 인 픽셀 수
long diff_outside(const Bitmap& a, const Bitmap& b, const std::vector<Rect>& masks) {
  if (a.width != b.width || a.height != b.height) return -2;
  long n = 0;
  for (int y = 0; y < a.height; ++y)
    for (int x = 0; x < a.width; ++x) {
      const double px = x / kScale, py = y / kScale;
      bool masked = false;
      for (const auto& m : masks)
        if (px >= m.x - 2 && px <= m.x + m.w + 2 && py >= m.y - 2 && py <= m.y + m.h + 2) masked = true;
      if (masked) continue;
      const auto* pa = a.row(y) + x * 4;
      const auto* pb = b.row(y) + x * 4;
      const int d = std::max({std::abs(pa[0] - pb[0]), std::abs(pa[1] - pb[1]), std::abs(pa[2] - pb[2])});
      if (d > 48) ++n;
    }
  return n;
}

RenderOptions opts() {
  RenderOptions o;
  o.scale = static_cast<float>(kScale);
  o.render_annotations = false;
  return o;
}

Stat finish(PdfiumDocument& doc, int page, const fs::path& tmp, const Bitmap& base, const std::vector<Rect>& masks) {
  Stat s;
  if (auto r = doc.save_to(tmp, false); !r) {
    s.error = "save: " + r.error().message;
    return s;
  }
  auto re = PdfiumDocument::open(tmp);
  if (!re) {
    s.error = "reopen";
    return s;
  }
  auto bmp = re->render_page(page, opts());
  if (!bmp) {
    s.error = "render";
    return s;
  }
  s.outside_big = diff_outside(base, *bmp, masks);
  return s;
}

std::string cell(const Stat& s) { return s.error.empty() ? std::to_string(s.outside_big) : s.error; }

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: onpdf_spike_m3_regen <out-dir> a.pdf ...\n";
    return 2;
  }
  const fs::path out = path_from_utf8(argv[1]);
  fs::create_directories(out);
  const fs::path tmp = out / "tmp.pdf";
  PdfiumLibrary lib;
  std::cout << "doc\tpage\timages\tadd\tmove\tdel\n";
  for (int i = 2; i < argc; ++i) {
    const fs::path file = path_from_utf8(argv[i]);
    const auto bytes = read_file(file);
    auto base_doc = PdfiumDocument::open_from_memory(bytes);
    if (!base_doc) continue;
    const std::string name = file.parent_path().filename().string() + "/" + file.filename().string();
    for (int p = 0; p < std::min(2, base_doc->page_count()); ++p) {
      auto base = base_doc->render_page(p, opts());
      if (!base) continue;
      auto objs = base_doc->page_objects(p);
      std::optional<PageObjectInfo> image;
      int images = 0;
      if (objs)
        for (const auto& o : *objs)
          if (o.kind == ObjectKind::kImage && !o.marked) {
            ++images;
            if (!image || o.bbox.w * o.bbox.h > image->bbox.w * image->bbox.h) image = o;
          }

      // add
      Stat add;
      {
        auto d = PdfiumDocument::open_from_memory(bytes);
        ImagePixels px{4, 4, std::vector<std::uint8_t>(64, 0)};
        for (std::size_t k = 0; k < px.bgra.size(); k += 4) px.bgra[k + 3] = 255;  // 불투명 검정
        const Rect r{5, 5, 10, 10};
        if (auto id = d->add_image_object(p, ImageSpec{px, r, {}}); id)
          add = finish(*d, p, tmp, *base, {r});
        else
          add.error = "add: " + id.error().message;
      }
      Stat move, del;
      if (image) {
        {
          auto d = PdfiumDocument::open_from_memory(bytes);
          auto id = d->mark_object(p, image->id);
          if (id && d->move_page_objects(p, {*id}, Point{1.0, 0.0})) {
            Rect moved = image->bbox;
            moved.x += 1;
            move = finish(*d, p, tmp, *base, {image->bbox, moved});
          } else {
            move.error = "move";
          }
        }
        {
          auto d = PdfiumDocument::open_from_memory(bytes);
          auto id = d->mark_object(p, image->id);
          if (id && d->detach_objects(p, {*id}))
            del = finish(*d, p, tmp, *base, {image->bbox});
          else
            del.error = "detach";
        }
      } else {
        move.error = del.error = "-";
      }
      std::cout << name << '\t' << p << '\t' << images << '\t' << cell(add) << '\t' << cell(move) << '\t' << cell(del) << std::endl;
    }
  }
  std::error_code ec;
  fs::remove(tmp, ec);
  return 0;
}
