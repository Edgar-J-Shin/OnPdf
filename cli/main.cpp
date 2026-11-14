// onpdf_cli — 렌더·검증용 명령줄 도구.
//   onpdf_cli info   <in.pdf>
//   onpdf_cli render <in.pdf> <out.png> [--page N] [--scale S] [--rotate 0|90|180|270]

#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/pdfium_library.h"
#include "onpdf/core/image/png_io.h"
#include "onpdf/core/platform/path.h"
#include "onpdf/core/view/tile.h"
#include "onpdf/core/view/tile_source.h"

#include <charconv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

using onpdf::engine::PdfiumDocument;
using onpdf::engine::RenderOptions;

int usage() {
  std::fputs(
      "usage:\n"
      "  onpdf_cli info   <in.pdf> [--password PW]\n"
      "  onpdf_cli render <in.pdf> <out.png> [--page N] [--scale S] [--rotate R] [--password PW]\n"
      "  onpdf_cli tiles  <in.pdf> <out-dir> [--page N] [--scale S] [--password PW]\n"
      "  onpdf_cli text   <in.pdf> [--page N] [--password PW]   (chars/lines/links + extracted text)\n"
      "                   (writes 512px tile PNGs, stitched.png and full.png)\n",
      stderr);
  return 2;
}

int fail(const onpdf::Error& e) {
  std::fprintf(stderr, "error: %s: %s\n", onpdf::to_string(e.code), e.message.c_str());
  return 1;
}

template <class T>
bool parse_number(std::string_view text, T& out) {
  const char* begin = text.data();
  const char* end = begin + text.size();
  auto [ptr, ec] = std::from_chars(begin, end, out);
  return ec == std::errc{} && ptr == end;
}

struct Args {
  std::string command;
  std::vector<std::string> positional;
  int page = 0;
  float scale = 1.0f;
  int rotate = 0;
  std::string password;
};

bool parse_args(int argc, char** argv, Args& args) {
  if (argc < 2) return false;
  args.command = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto next = [&](std::string_view& value) {
      if (i + 1 >= argc) return false;
      value = argv[++i];
      return true;
    };
    std::string_view v;
    if (a == "--page") {
      if (!next(v) || !parse_number(v, args.page)) return false;
    } else if (a == "--scale") {
      if (!next(v)) return false;
      try {
        args.scale = std::stof(std::string(v));
      } catch (...) {
        return false;
      }
    } else if (a == "--rotate") {
      if (!next(v) || !parse_number(v, args.rotate)) return false;
    } else if (a == "--password") {
      if (!next(v)) return false;
      args.password = std::string(v);
    } else if (a.starts_with("--")) {
      return false;
    } else {
      args.positional.emplace_back(a);
    }
  }
  return true;
}

int cmd_info(const Args& args) {
  if (args.positional.size() != 1) return usage();
  auto doc = PdfiumDocument::open(onpdf::path_from_utf8(args.positional[0]), args.password);
  if (!doc) return fail(doc.error());
  const int count = doc->page_count();
  std::printf("file: %s\npages: %d\n", args.positional[0].c_str(), count);
  for (int i = 0; i < count; ++i) {
    auto size = doc->page_size(i);
    if (!size) return fail(size.error());
    std::printf("  page %d: %.2f x %.2f pt\n", i, size->width, size->height);
  }
  return 0;
}

int cmd_render(const Args& args) {
  if (args.positional.size() != 2) return usage();
  auto doc = PdfiumDocument::open(onpdf::path_from_utf8(args.positional[0]), args.password);
  if (!doc) return fail(doc.error());

  RenderOptions options;
  options.scale = args.scale;
  options.rotation = args.rotate;
  auto bitmap = doc->render_page(args.page, options);
  if (!bitmap) return fail(bitmap.error());

  auto written = onpdf::image::write_png(*bitmap, onpdf::path_from_utf8(args.positional[1]));
  if (!written) return fail(written.error());
  std::printf("rendered page %d -> %s (%d x %d)\n", args.page, args.positional[1].c_str(),
              bitmap->width, bitmap->height);
  return 0;
}

int cmd_tiles(const Args& args) {
  if (args.positional.size() != 2) return usage();
  auto doc = PdfiumDocument::open(onpdf::path_from_utf8(args.positional[0]), args.password);
  if (!doc) return fail(doc.error());
  auto size = doc->page_size(args.page);
  if (!size) return fail(size.error());

  const std::filesystem::path out_dir = onpdf::path_from_utf8(args.positional[1]);
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  if (ec) {
    std::fprintf(stderr, "error: cannot create %s\n", args.positional[1].c_str());
    return 1;
  }
  const int scale_q = onpdf::view::quantize_scale(args.scale);
  const onpdf::Size px = onpdf::view::page_pixel_size(*size, scale_q);
  onpdf::Bitmap stitched;
  stitched.width = static_cast<int>(px.w);
  stitched.height = static_cast<int>(px.h);
  stitched.stride = stitched.width * 4;
  stitched.pixels.assign(static_cast<std::size_t>(stitched.stride) * static_cast<std::size_t>(stitched.height), 255);

  onpdf::view::SyncTileSource source(*doc);
  int count = 0;
  int failed = 0;
  for (int ty = 0; ty < onpdf::view::tiles_across(px.h); ++ty) {
    for (int tx = 0; tx < onpdf::view::tiles_across(px.w); ++tx) {
      const onpdf::view::TileKey key{args.page, scale_q, tx, ty};
      source.request({key, *size}, onpdf::engine::Priority::kHigh,
                     [&](const onpdf::view::TileKey&, onpdf::Result<onpdf::Bitmap> r) {
                       if (!r) {
                         ++failed;
                         return;
                       }
                       const onpdf::Rect rect = onpdf::view::tile_rect_px(key, *size);
                       for (int y = 0; y < r->height; ++y) {
                         std::memcpy(stitched.pixels.data() +
                                         static_cast<std::size_t>(static_cast<int>(rect.y) + y) * static_cast<std::size_t>(stitched.stride) +
                                         static_cast<std::size_t>(rect.x) * 4,
                                     r->row(y), static_cast<std::size_t>(r->width) * 4);
                       }
                       const auto name = "tile-" + std::to_string(tx) + "-" + std::to_string(ty) + ".png";
                       if (!onpdf::image::write_png(*r, out_dir / name)) ++failed;
                       ++count;
                     });
    }
  }
  auto written = onpdf::image::write_png(stitched, out_dir / "stitched.png");
  if (!written) return fail(written.error());
  // 비교용: 쪽 전체를 한 영역으로 렌더
  onpdf::engine::RenderOptions full_opts;
  full_opts.scale = static_cast<float>(onpdf::view::dequantize_scale(scale_q));
  auto full = doc->render_region(args.page, full_opts, onpdf::Rect{0, 0, px.w, px.h});
  if (full) (void)onpdf::image::write_png(*full, out_dir / "full.png");
  std::printf("page %d @%.3f: %d tiles (%d failed) -> %s\n", args.page, onpdf::view::dequantize_scale(scale_q),
              count, failed, args.positional[1].c_str());
  return failed == 0 ? 0 : 1;
}

int cmd_text(const Args& args) {
  if (args.positional.size() != 1) return usage();
  auto doc = PdfiumDocument::open(onpdf::path_from_utf8(args.positional[0]), args.password);
  if (!doc) return fail(doc.error());
  auto index = doc->load_text(args.page);
  if (!index) return fail(index.error());
  std::printf("page %d: chars=%d lines=%zu links=%zu\n", args.page, index->char_count(), index->lines.size(),
              index->links.size());
  for (const auto& link : index->links) {
    if (link.kind == onpdf::view::LinkKind::kUri) {
      std::printf("  link uri %s%s\n", link.uri.c_str(), onpdf::view::is_safe_uri(link.uri) ? "" : " (blocked)");
    } else {
      std::printf("  link page %d%s\n", link.page, link.location ? " (with location)" : "");
    }
  }
  const std::string text = index->all_text();
  std::fwrite(text.data(), 1, text.size(), stdout);
  if (!text.empty() && text.back() != '\n') std::printf("\n");
  return 0;
}

}  // namespace

int run(int argc, char** argv) {
  Args args;
  if (!parse_args(argc, argv, args)) return usage();

  onpdf::engine::PdfiumLibrary library;
  if (args.command == "info") return cmd_info(args);
  if (args.command == "render") return cmd_render(args);
  if (args.command == "tiles") return cmd_tiles(args);
  if (args.command == "text") return cmd_text(args);
  return usage();
}

#ifdef _WIN32
// Windows 콘솔 인자는 코드 페이지 인코딩이므로 wmain 으로 받아 UTF-8 로 바꾼다.
int wmain(int argc, wchar_t** argv) {
  std::vector<std::string> utf8;
  std::vector<char*> ptrs;
  for (int i = 0; i < argc; ++i) utf8.push_back(onpdf::path_to_utf8(std::filesystem::path(argv[i])));
  for (auto& s : utf8) ptrs.push_back(s.data());
  return run(argc, ptrs.data());
}
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
