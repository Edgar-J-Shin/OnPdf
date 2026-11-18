// onpdf_cli — 렌더·검증용 명령줄 도구.
//   onpdf_cli info   <in.pdf>
//   onpdf_cli render <in.pdf> <out.png> [--page N] [--scale S] [--rotate 0|90|180|270]

#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/pdfium_library.h"
#include "onpdf/core/engine/qpdf_tools.h"
#include "onpdf/core/engine/save.h"
#include "onpdf/core/page_ranges.h"
#include "onpdf/core/image/png_io.h"
#include "onpdf/core/platform/path.h"
#include "onpdf/core/view/tile.h"
#include "onpdf/core/view/tile_source.h"

#include <algorithm>
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
      "  onpdf_cli render <in.pdf> <out.png> [--page N] [--scale S] [--rotate R] [--print] [--password PW]\n"
      "  onpdf_cli tiles  <in.pdf> <out-dir> [--page N] [--scale S] [--password PW]\n"
      "  onpdf_cli text   <in.pdf> [--page N] [--password PW]   (chars/lines/links + extracted text)\n"
      "                   (writes 512px tile PNGs, stitched.png and full.png)\n"
      "  onpdf_cli search <in.pdf> <query> [--case] [--word] [--password PW]\n"
      "  onpdf_cli outline <in.pdf> [--password PW]\n"
      "  onpdf_cli merge  <out.pdf> <in.pdf>... [--password PW]      (첫 파일의 목차·폼 유지)\n"
      "  onpdf_cli split  <in.pdf> <out-dir> (--every N | --ranges \"1-3,5\" | --each) [--pattern NAME-%d.pdf] [--password PW]\n"
      "  onpdf_cli check  <in.pdf> [--password PW]                   (qpdf 구조 검사, 경고는 출력하고 0)\n",
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
  bool match_case = false;
  bool whole_word = false;
  bool printing = false;
  int every = 0;
  std::string ranges;
  bool each = false;
  std::string pattern;
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
    } else if (a == "--case") {
      args.match_case = true;
    } else if (a == "--word") {
      args.whole_word = true;
    } else if (a == "--print") {
      args.printing = true;
    } else if (a == "--password") {
      if (!next(v)) return false;
      args.password = std::string(v);
    } else if (a == "--every") {
      if (!next(v) || !parse_number(v, args.every)) return false;
    } else if (a == "--ranges") {
      if (!next(v)) return false;
      args.ranges = std::string(v);
    } else if (a == "--each") {
      args.each = true;
    } else if (a == "--pattern") {
      if (!next(v)) return false;
      args.pattern = std::string(v);
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
  auto print = doc->print_preferences();
  if (!print) return fail(print.error());
  std::string ranges;
  for (const auto& [first, last] : print->page_ranges) {
    if (!ranges.empty()) ranges += ",";
    ranges += std::to_string(first + 1) + "-" + std::to_string(last + 1);
  }
  const char* duplex = "-";
  switch (print->duplex) {
    case onpdf::view::Duplex::kSimplex: duplex = "simplex"; break;
    case onpdf::view::Duplex::kFlipShortEdge: duplex = "short-edge"; break;
    case onpdf::view::Duplex::kFlipLongEdge: duplex = "long-edge"; break;
    case onpdf::view::Duplex::kUndefined: break;
  }
  std::printf("print: %s · scaling %s · copies %d · ranges %s · duplex %s\n",
              !print->allowed ? "denied" : (print->high_quality ? "allowed" : "low-res only"),
              print->no_scaling ? "none" : "default", print->copies, ranges.empty() ? "all" : ranges.c_str(), duplex);
  return 0;
}

int cmd_render(const Args& args) {
  if (args.positional.size() != 2) return usage();
  auto doc = PdfiumDocument::open(onpdf::path_from_utf8(args.positional[0]), args.password);
  if (!doc) return fail(doc.error());

  RenderOptions options;
  options.scale = args.scale;
  options.rotation = args.rotate;
  options.printing = args.printing;
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
  auto printable = [](const std::string& raw) {  // 터미널 이스케이프 주입 방지
    std::string out;
    for (char ch : raw) {
      const auto c = static_cast<unsigned char>(ch);
      if (c < 0x20 || c == 0x7F) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "\\x%02X", c);
        out += buf;
      } else {
        out += static_cast<char>(c);
      }
    }
    return out;
  };
  for (const auto& link : index->links) {
    if (link.kind == onpdf::view::LinkKind::kUri) {
      std::printf("  link uri %s%s\n", printable(link.uri).c_str(),
                  onpdf::view::is_safe_uri(link.uri) ? "" : " (blocked)");
    } else {
      std::printf("  link page %d%s\n", link.page, link.location ? " (with location)" : "");
    }
  }
  const std::string text = index->all_text();
  std::fwrite(text.data(), 1, text.size(), stdout);
  if (!text.empty() && text.back() != '\n') std::printf("\n");
  return 0;
}

int cmd_search(const Args& args) {
  if (args.positional.size() != 2) return usage();
  auto doc = PdfiumDocument::open(onpdf::path_from_utf8(args.positional[0]), args.password);
  if (!doc) return fail(doc.error());
  onpdf::view::SearchOptions options;
  options.match_case = args.match_case;
  options.whole_word = args.whole_word;
  const std::string query = onpdf::view::normalize_query(args.positional[1]);
  if (query.empty()) {
    std::fprintf(stderr, "error: empty query\n");
    return 2;
  }
  std::size_t total = 0;
  for (int page = 0; page < doc->page_count(); ++page) {
    auto r = doc->search_page(page, query, options);
    if (!r) return fail(r.error());
    if (r->ranges.empty()) continue;
    std::printf("page %d: %zu hit(s)\n", page + 1, r->ranges.size());
    auto index = doc->load_text(page);  // 문맥 출력용
    if (!index) return fail(index.error());
    for (const auto& range : r->ranges) {
      const int from = std::max(0, range.start - 20);
      const int to = std::min(index->char_count(), range.end + 20);
      std::string context = index->text({from, to});
      for (char& c : context) {
        if (c == '\n') c = ' ';
      }
      std::printf("  ...%s...\n", context.c_str());
    }
    total += r->ranges.size();
  }
  std::printf("%zu hit(s) for \"%s\"\n", total, query.c_str());
  return 0;
}

int cmd_outline(const Args& args) {
  if (args.positional.size() != 1) return usage();
  auto doc = PdfiumDocument::open(onpdf::path_from_utf8(args.positional[0]), args.password);
  if (!doc) return fail(doc.error());
  auto outline = doc->load_outline();
  if (!outline) return fail(outline.error());
  for (const auto& item : outline->items) {
    std::string target = "-";
    if (item.target) {
      target = item.target->kind == onpdf::view::LinkKind::kPage ? "p." + std::to_string(item.target->page + 1)
                                                                 : "uri";
    }
    std::printf("%*s%s%s [%s]\n", item.depth * 2, "", item.open ? "" : "+ ",
                item.title.empty() ? "(untitled)" : item.title.c_str(), target.c_str());
  }
  std::printf("%d item(s)%s%s\n", outline->size(), outline->truncated ? " (truncated)" : "",
              outline->cycle_detected ? " (cycle detected)" : "");
  return 0;
}

}  // namespace

void print_warnings(const onpdf::engine::JobReport& report) {
  for (const auto& w : report.warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
}

// 병합: 첫 파일이 주 입력. 임시 → 검사·쪽 수 → 교체
int cmd_merge(const Args& args) {
  if (args.positional.size() < 2) return usage();
  using onpdf::engine::QpdfTools;
  const auto out = onpdf::path_from_utf8(args.positional[0]);
  std::vector<onpdf::engine::StructureInput> inputs;
  int expected = 0;
  for (std::size_t i = 1; i < args.positional.size(); ++i) {
    onpdf::engine::StructureInput in{onpdf::path_from_utf8(args.positional[i]), args.password};
    auto n = QpdfTools::page_count(in.path, in.password);
    if (!n) return fail(n.error());
    expected += *n;
    inputs.push_back(std::move(in));
  }
  onpdf::engine::JobReport report;
  onpdf::engine::SafeWriteHooks hooks;
  hooks.write = [&](const std::vector<std::filesystem::path>& temps) -> onpdf::Result<void> {
    auto r = QpdfTools::merge(inputs, temps.front());
    if (!r) return std::unexpected(r.error());
    report = std::move(*r);
    return {};
  };
  hooks.verify = [&](const std::filesystem::path&, const std::filesystem::path& temp) -> onpdf::Result<void> {
    if (auto c = QpdfTools::check(temp); !c) return std::unexpected(c.error());
    auto n = QpdfTools::page_count(temp);
    if (!n) return std::unexpected(n.error());
    if (*n != expected) return onpdf::make_error(onpdf::ErrorCode::kSaveVerifyFailed, "merged page count mismatch");
    return {};
  };
  auto saved = onpdf::engine::write_files_safely({out}, hooks);
  if (!saved) return fail(saved.error());
  print_warnings(report);
  std::printf("merged %zu files, %d pages -> %s\n", inputs.size(), expected, onpdf::path_to_utf8(out).c_str());
  return 0;
}

// 분할: 범위마다 파일 하나. 전부 검증한 뒤 한 번에 교체
int cmd_split(const Args& args) {
  if (args.positional.size() != 2) return usage();
  using onpdf::engine::QpdfTools;
  const onpdf::engine::StructureInput in{onpdf::path_from_utf8(args.positional[0]), args.password};
  const auto dir = onpdf::path_from_utf8(args.positional[1]);
  auto count = QpdfTools::page_count(in.path, in.password);
  if (!count) return fail(count.error());
  std::vector<onpdf::PageRange> ranges;
  if (args.every > 0) {
    ranges = onpdf::ranges_every(args.every, *count);
  } else if (!args.ranges.empty()) {
    auto parsed = onpdf::parse_page_ranges(args.ranges, *count);
    if (!parsed) return fail(parsed.error());
    ranges = std::move(*parsed);
  } else if (args.each) {
    ranges = onpdf::ranges_each(*count);
  } else {
    return usage();
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::string stem = onpdf::path_to_utf8(in.path.stem());
  const std::string pattern = args.pattern.empty() ? stem + "-%d.pdf" : args.pattern;
  std::vector<std::filesystem::path> outs;
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    outs.push_back(dir / onpdf::path_from_utf8(onpdf::output_name(pattern, static_cast<int>(i) + 1,
                                                                   static_cast<int>(ranges.size()), ranges[i], *count)));
  }
  std::vector<std::string> warnings;
  onpdf::engine::SafeWriteHooks hooks;
  hooks.write = [&](const std::vector<std::filesystem::path>& temps) -> onpdf::Result<void> {
    for (std::size_t i = 0; i < temps.size(); ++i) {
      auto r = QpdfTools::extract(in, ranges[i], temps[i]);
      if (!r) return std::unexpected(r.error());
      warnings.insert(warnings.end(), r->warnings.begin(), r->warnings.end());
    }
    return {};
  };
  hooks.verify = [&](const std::filesystem::path& target, const std::filesystem::path& temp) -> onpdf::Result<void> {
    const auto it = std::find(outs.begin(), outs.end(), target);
    const std::size_t i = static_cast<std::size_t>(it - outs.begin());
    auto n = QpdfTools::page_count(temp);
    if (!n) return std::unexpected(n.error());
    if (*n != ranges[i].last - ranges[i].first + 1) {
      return onpdf::make_error(onpdf::ErrorCode::kSaveVerifyFailed, "split page count mismatch");
    }
    return {};
  };
  auto saved = onpdf::engine::write_files_safely(outs, hooks);
  if (!saved) return fail(saved.error());
  for (const auto& w : warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
  for (std::size_t i = 0; i < outs.size(); ++i) {
    std::printf("%s  pages %s\n", onpdf::path_to_utf8(outs[i]).c_str(), onpdf::range_text(ranges[i]).c_str());
  }
  std::printf("%zu files\n", outs.size());
  return 0;
}

int cmd_check(const Args& args) {
  if (args.positional.size() != 1) return usage();
  auto report = onpdf::engine::QpdfTools::check(onpdf::path_from_utf8(args.positional[0]), args.password);
  if (!report) return fail(report.error());
  print_warnings(*report);
  std::printf("ok (%zu warnings)\n", report->warnings.size());
  return 0;
}

int run(int argc, char** argv) {
  Args args;
  if (!parse_args(argc, argv, args)) return usage();

  onpdf::engine::PdfiumLibrary library;
  if (args.command == "info") return cmd_info(args);
  if (args.command == "render") return cmd_render(args);
  if (args.command == "tiles") return cmd_tiles(args);
  if (args.command == "text") return cmd_text(args);
  if (args.command == "search") return cmd_search(args);
  if (args.command == "outline") return cmd_outline(args);
  if (args.command == "merge") return cmd_merge(args);
  if (args.command == "split") return cmd_split(args);
  if (args.command == "check") return cmd_check(args);
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
