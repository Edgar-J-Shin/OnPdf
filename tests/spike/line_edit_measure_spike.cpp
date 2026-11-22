// Spike #75: M4 완료 기준 측정 — 제품 파이프라인(text_lines → ReplaceTextLineCommand)으로 코퍼스의
// 줄을 고쳐 (1) 줄 밖 픽셀 동일 (2) 원본 글꼴 재사용 줄의 폭 비율 (3) 거절 사유를 센다. 제품 코드가
// 아니다.
//
// 실행: onpdf_spike_line_edit_measure <out.tsv> [--pages N] [--lines K] [--catalog] a.pdf b.pdf …
//   --catalog: 시스템 글꼴 카탈로그로 대체 글꼴을 고른다 (앱과 같은 규칙), 없으면 저장소 서브셋
//   Noto
#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/pdfium_library.h"
#include "onpdf/core/platform/font_dirs.h"
#include "onpdf/core/platform/path.h"
#include "onpdf/edit/font_catalog.h"
#include "onpdf/edit/font_face.h"
#include "onpdf/edit/text_line_commands.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace onpdf;
using engine::PdfiumDocument;
using engine::TextLineInfo;

struct Options {
  int pages = 2;
  int lines_per_page = 3;
  bool catalog = false;
};

Bitmap render(const PdfiumDocument& doc, int page) {
  engine::RenderOptions o;
  o.scale = 1.0f;
  o.render_annotations = false;
  auto b = doc.render_page(page, o);
  return b ? *b : Bitmap{};
}

// 쪽 포인트 사각형(여유 pad) 밖에서 다른 픽셀 수 (배율 1)
long diff_outside(const Bitmap& a, const Bitmap& b, const Rect& mask, double pad) {
  if (a.width != b.width || a.height != b.height || a.width == 0) return -1;
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

std::size_t utf8_chars(const std::string& s) {
  std::size_t n = 0;
  for (const unsigned char c : s)
    if ((c & 0xC0) != 0x80) ++n;
  return n;
}

bool has_hangul(const std::string& s) {
  for (std::size_t i = 0; i + 2 < s.size(); ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c >= 0xEA && c <= 0xED) return true;  // U+AC00..U+D7A3 의 첫 바이트 범위 (대략)
  }
  return false;
}

// 대상 줄: 가로·10자 이상, 위→아래 25/50/75% 에 가장 가까운 줄
std::vector<const TextLineInfo*> pick_lines(const std::vector<TextLineInfo>& lines,
                                            double page_height, int count) {
  std::vector<const TextLineInfo*> candidates;
  for (const auto& l : lines) {
    if (utf8_chars(l.text) >= 10 && l.bbox.w > l.bbox.h) candidates.push_back(&l);
  }
  std::vector<const TextLineInfo*> out;
  for (int k = 1; k <= count; ++k) {
    const double target = page_height * k / (count + 1);
    const TextLineInfo* best = nullptr;
    for (const auto* c : candidates) {
      bool taken = false;
      for (const auto* o : out)
        if (o == c) taken = true;
      if (taken) continue;
      if (best == nullptr || std::abs(c->bbox.y - target) < std::abs(best->bbox.y - target))
        best = c;
    }
    if (best != nullptr) out.push_back(best);
  }
  return out;
}

struct Picker {
  std::shared_ptr<const edit::FontFace> bundle;
  std::shared_ptr<const edit::FontCatalog> catalog;
  std::map<std::string, std::shared_ptr<const edit::FontFace>> cache;

  // 앱 fallback_font_for 와 같은 규칙
  std::shared_ptr<const edit::FontFace> pick(const std::string& font_name, const std::string& text,
                                             std::string* chosen) {
    *chosen = bundle ? bundle->family_name() : "";
    if (!catalog) return bundle;
    std::vector<const edit::FontEntry*> candidates = catalog->match(font_name, true);
    if (has_hangul(text)) {
      if (const auto* sans = catalog->default_hangul_sans(true); sans != nullptr)
        candidates.push_back(sans);
    }
    for (const auto* e : candidates) {
      const std::string key = path_to_utf8(e->path) + "#" + std::to_string(e->face_index);
      auto it = cache.find(key);
      if (it == cache.end()) {
        auto face = edit::FontFace::load(e->path, e->face_index);
        it = cache.emplace(key, face ? *face : nullptr).first;
      }
      if (it->second && it->second->missing(text).empty()) {
        *chosen = it->second->family_name();
        return it->second;
      }
    }
    return bundle;
  }
};

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  std::vector<std::filesystem::path> files;
  std::filesystem::path out_tsv;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--pages" && i + 1 < argc)
      opt.pages = std::atoi(argv[++i]);
    else if (a == "--lines" && i + 1 < argc)
      opt.lines_per_page = std::atoi(argv[++i]);
    else if (a == "--catalog")
      opt.catalog = true;
    else if (out_tsv.empty())
      out_tsv = path_from_utf8(a);
    else
      files.push_back(path_from_utf8(a));
  }
  if (out_tsv.empty() || files.empty()) {
    std::cerr << "usage: onpdf_spike_line_edit_measure <out.tsv> [--pages N] [--lines K] "
                 "[--catalog] a.pdf ...\n";
    return 2;
  }
  engine::PdfiumLibrary::Options lib;
  lib.use_system_fonts = true;  // 실제 앱과 같게 (비임베딩 글꼴 렌더)
  engine::PdfiumLibrary library(lib);
  Picker picker;
  if (auto bundle = edit::FontFace::load(path_from_utf8(ONPDF_TESTDATA_DIR) / "fonts" /
                                         "NotoSansKR-subset.ttf"))
    picker.bundle = *bundle;
  if (opt.catalog)
    picker.catalog = std::make_shared<const edit::FontCatalog>(
        edit::FontCatalog::scan(platform::system_font_dirs()));

  std::ofstream tsv(out_tsv);
  tsv << "file\tpage\tline\tfont\tembedded\ttype3\tobjects\tchars\teditable\treason\tresult\tused_"
         "fallback\tfallback_font\t"
         "width_ratio\tstart_dx\tpixels_outside\tms\ttext\n";
  int lines_total = 0, uneditable = 0, refused = 0, ok = 0, ofont = 0, fallback = 0, pix_same = 0,
      w5 = 0, w2 = 0, w10 = 0;
  std::map<std::string, int> reasons;
  std::vector<double> ratios;
  for (const auto& file : files) {
    auto doc = PdfiumDocument::open(file);
    if (!doc) {
      std::cerr << "skip " << file << ": " << doc.error().message << "\n";
      continue;
    }
    const int pages = std::min(opt.pages, doc->page_count());
    for (int p = 0; p < pages; ++p) {
      auto lines = doc->text_lines(p);
      if (!lines) continue;
      const auto size = doc->page_size(p);
      const auto picked = pick_lines(*lines, size ? size->height : 792.0, opt.lines_per_page);
      for (const auto* line : picked) {
        const TextLineInfo info = *line;  // 수정 뒤 목록이 바뀐다
        ++lines_total;
        tsv << path_to_utf8(file.filename()) << '\t' << p << '\t' << info.id << '\t'
            << info.font_name << '\t' << info.font_embedded << '\t' << info.font_type3 << '\t'
            << info.object_count << '\t' << utf8_chars(info.text) << '\t' << info.editable << '\t'
            << info.reason << '\t';
        if (!info.editable) {
          ++uneditable;
          ++reasons[info.reason];
          tsv << "uneditable\t\t\t\t\t\t\t" << info.text.substr(0, 60) << "\n";
          continue;
        }
        std::string chosen;
        auto face = picker.pick(info.font_name, info.text, &chosen);
        const Bitmap before = render(*doc, p);
        const auto t0 = std::chrono::steady_clock::now();
        // 같은 글로 교체: 폭 비율은 1.0 이 이상적 (원본 글꼴 재사용이면 TJ 커닝·Tc/Tw 만큼
        // 달라진다)
        edit::ReplaceTextLineCommand cmd(p, info.id, info.text, std::nullopt, std::nullopt, face);
        auto exec = cmd.execute(*doc);
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
        if (!exec) {
          ++refused;
          ++reasons["refused: " + exec.error().message.substr(0, 40)];
          tsv << "refused: " << exec.error().message << "\t\t\t\t\t\t" << ms << '\t'
              << info.text.substr(0, 60) << "\n";
          continue;
        }
        ++ok;
        if (cmd.used_fallback())
          ++fallback;
        else
          ++ofont;
        const Bitmap after = render(*doc, p);
        double ratio = 0.0, dx = 0.0;
        Rect mask = info.bbox;
        if (auto objects = doc->text_objects(p)) {
          Rect box;
          bool found = false;
          for (const auto& o : *objects) {
            if (o.box != cmd.box()) continue;
            if (!found) {
              box = o.bbox;
              found = true;
            } else {
              const double x0 = std::min(box.x, o.bbox.x),
                           x1 = std::max(box.right(), o.bbox.right());
              box = {x0, box.y, x1 - x0, box.h};
            }
          }
          if (found && info.bbox.w > 0) {
            ratio = box.w / info.bbox.w;
            dx = box.x - info.bbox.x;
            // 새 상자가 원래 줄보다 넓으면 그만큼은 "줄 안" — 줄 밖 비교 마스크는 둘의 합집합
            const double x0 = std::min(mask.x, box.x), y0 = std::min(mask.y, box.y);
            const double x1 = std::max(mask.right(), box.right()),
                         y1 = std::max(mask.bottom(), box.bottom());
            mask = {x0, y0, x1 - x0, y1 - y0};
          }
        }
        const long outside = diff_outside(before, after, mask, info.font_size * 0.35 + 1.0);
        if (outside == 0) ++pix_same;
        if (!cmd.used_fallback() && ratio > 0) {
          ratios.push_back(ratio);
          if (std::abs(ratio - 1.0) <= 0.02) ++w2;
          if (std::abs(ratio - 1.0) <= 0.05) ++w5;
          if (std::abs(ratio - 1.0) <= 0.10) ++w10;
        }
        tsv << "ok\t" << cmd.used_fallback() << '\t' << chosen << '\t' << ratio << '\t' << dx
            << '\t' << outside << '\t' << ms << '\t' << info.text.substr(0, 60) << "\n";
        // 되돌려 다음 줄의 id 가 유지되게
        (void)cmd.undo(*doc);
        cmd.discard(*doc);
      }
    }
  }
  std::sort(ratios.begin(), ratios.end());
  std::cout << "lines=" << lines_total << " uneditable=" << uneditable << " refused=" << refused
            << " ok=" << ok << " (ofont " << ofont << ", fallback " << fallback << ")\n";
  std::cout << "pixels outside identical: " << pix_same << "/" << ok << "\n";
  if (!ratios.empty()) {
    std::cout << "ofont width ratio: n=" << ratios.size() << " median=" << ratios[ratios.size() / 2]
              << " ±2% " << w2 << " ±5% " << w5 << " ±10% " << w10 << " min=" << ratios.front()
              << " max=" << ratios.back() << "\n";
  }
  for (const auto& [reason, n] : reasons) std::cout << "  " << n << "\t" << reason << "\n";
  return 0;
}
