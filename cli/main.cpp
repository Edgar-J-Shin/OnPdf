// onpdf_cli — 렌더·검증용 명령줄 도구.
//   onpdf_cli info   <in.pdf>
//   onpdf_cli render <in.pdf> <out.png> [--page N] [--scale S] [--rotate 0|90|180|270]

#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/pdfium_library.h"
#include "onpdf/core/image/png_io.h"
#include "onpdf/core/platform/path.h"

#include <charconv>
#include <cstdio>
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
      "  onpdf_cli render <in.pdf> <out.png> [--page N] [--scale S] [--rotate R] [--password PW]\n",
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

}  // namespace

int run(int argc, char** argv) {
  Args args;
  if (!parse_args(argc, argv, args)) return usage();

  onpdf::engine::PdfiumLibrary library;
  if (args.command == "info") return cmd_info(args);
  if (args.command == "render") return cmd_render(args);
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
