#include "onpdf/edit/font_catalog.h"

#include "onpdf/core/platform/path.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <hb-ot.h>
#include <hb.h>
#include <iterator>
#include <memory>

namespace onpdf::edit {
namespace {

struct BlobDeleter {
  void operator()(hb_blob_t* b) const noexcept { hb_blob_destroy(b); }
};
struct FaceDeleter {
  void operator()(hb_face_t* f) const noexcept { hb_face_destroy(f); }
};
struct FontDeleter {
  void operator()(hb_font_t* f) const noexcept { hb_font_destroy(f); }
};
using Blob = std::unique_ptr<hb_blob_t, BlobDeleter>;
using Face = std::unique_ptr<hb_face_t, FaceDeleter>;
using Font = std::unique_ptr<hb_font_t, FontDeleter>;

std::string name_of(hb_face_t* face, hb_ot_name_id_t id) {
  char buffer[256] = {};
  unsigned len = sizeof buffer;
  hb_ot_name_get_utf8(face, id, HB_LANGUAGE_INVALID, &len, buffer);
  return buffer;
}

bool font_extension(const std::filesystem::path& p) {
  std::string ext = onpdf::path_to_utf8(p.extension());
  for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return ext == ".ttf" || ext == ".otf" || ext == ".ttc";
}

// mmap 으로 연다 (HarfBuzz: POSIX mmap, Windows 는 UTF-8 → CreateFileW + 파일 매핑). 전체를 읽으면
// 글꼴 폴더가 수백 MB 라 느리다
Blob read_blob(const std::filesystem::path& p) {
  return Blob(hb_blob_create_from_file_or_fail(onpdf::path_to_utf8(p).c_str()));
}

void scan_file(const std::filesystem::path& p, unsigned min_glyphs, std::vector<FontEntry>& out) {
  Blob blob = read_blob(p);
  if (!blob) return;
  const unsigned count = hb_face_count(blob.get());
  for (unsigned i = 0; i < count; ++i) {
    Face face(hb_face_create(blob.get(), i));
    if (!face) continue;
    FontEntry e;
    e.path = p;
    e.face_index = i;
    e.glyph_count = hb_face_get_glyph_count(face.get());
    if (e.glyph_count < min_glyphs) continue;
    e.family = name_of(face.get(), HB_OT_NAME_ID_FONT_FAMILY);
    if (e.family.empty() || e.family.front() == '.') continue;  // macOS 숨김 글꼴
    e.style = name_of(face.get(), HB_OT_NAME_ID_FONT_SUBFAMILY);
    for (const hb_tag_t tag : {HB_TAG('C', 'F', 'F', ' '), HB_TAG('C', 'F', 'F', '2')}) {
      Blob table(hb_face_reference_table(face.get(), tag));
      if (table && hb_blob_get_length(table.get()) > 0) e.cff = true;
    }
    Font font(hb_font_create(face.get()));
    hb_codepoint_t gid = 0;
    e.hangul = hb_font_get_nominal_glyph(font.get(), U'한', &gid) && gid != 0 &&
               hb_font_get_nominal_glyph(font.get(), U'글', &gid) && gid != 0;
    e.weight = static_cast<int>(hb_style_get_value(font.get(), HB_STYLE_TAG_WEIGHT));
    if (e.weight <= 0) e.weight = 400;
    out.push_back(std::move(e));
  }
}

// 소문자·구분자 제거만 (스타일 비교용 — normalize_name 은 family 용이라 스타일 접미를 뗀다)
std::string fold(std::string_view name) {
  std::string out;
  for (const char ch : name) {
    if (ch == ' ' || ch == '-' || ch == '_') continue;
    out += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return out;
}

bool is_style_word(std::string_view folded) {
  for (const char* w : {"roman", "oblique", "bolditalic", "boldoblique", "bold", "italic",
                        "regular", "medium", "light", "semibold", "black", "heavy"}) {
    if (folded == w) return true;
  }
  return false;
}

bool is_regular(const FontEntry& e) {
  const std::string s = fold(e.style);
  return (s.empty() || s == "regular" || s == "normal" || s == "book" || s == "roman") &&
         e.weight >= 350 && e.weight <= 450;
}

bool better(const FontEntry& a, const FontEntry& b) {
  const bool ra = is_regular(a), rb = is_regular(b);
  if (ra != rb) return ra;
  if (a.cff != b.cff) return !a.cff;
  if (a.path != b.path) return a.path < b.path;
  return a.face_index < b.face_index;
}

}  // namespace

FontCatalog FontCatalog::scan(const std::vector<platform::FontDir>& dirs, unsigned min_glyphs,
                              const std::function<bool()>& cancelled) {
  FontCatalog catalog;
  for (const auto& dir : dirs) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir.path, ec)) continue;
    const auto visit = [&](const std::filesystem::path& file) {
      if (cancelled && cancelled()) return;
      std::error_code fec;
      if (std::filesystem::is_regular_file(file, fec) && font_extension(file))
        scan_file(file, min_glyphs, catalog.entries_);
    };
    if (dir.recursive) {
      for (std::filesystem::recursive_directory_iterator
               it(dir.path, std::filesystem::directory_options::skip_permission_denied, ec),
           end;
           !ec && it != end; it.increment(ec)) {
        visit(it->path());
      }
    } else {
      for (std::filesystem::directory_iterator
               it(dir.path, std::filesystem::directory_options::skip_permission_denied, ec),
           end;
           !ec && it != end; it.increment(ec)) {
        visit(it->path());
      }
    }
  }
  std::sort(catalog.entries_.begin(), catalog.entries_.end(),
            [](const FontEntry& a, const FontEntry& b) {
              return a.path != b.path ? a.path < b.path : a.face_index < b.face_index;
            });
  return catalog;
}

FontCatalog FontCatalog::from_entries(std::vector<FontEntry> entries) {
  FontCatalog catalog;
  catalog.entries_ = std::move(entries);
  return catalog;
}

std::string FontCatalog::normalize_name(std::string_view name) {
  std::string s(name);
  if (s.size() > 7 && s[6] == '+') s.erase(0, 7);  // 서브셋 태그 ABCDEF+
  if (const auto comma = s.find(','); comma != std::string::npos) s.erase(comma);  // "Arial,Bold"
  for (const char* suffix : {"-Regular", "PSMT", "MT", "PS"}) {
    const std::string_view sv(suffix);
    if (s.size() > sv.size() && s.compare(s.size() - sv.size(), sv.size(), sv) == 0) {
      s.erase(s.size() - sv.size());
      break;
    }
  }
  // Base14 식 "-스타일" 접미는 뗀다: "Times-Roman" → "times", "Courier-BoldOblique" → "courier"
  // (리뷰 I1). 하이픈 뒤가 스타일 단어일 때만 (family 가 "bold" 로 끝나는 이름을 자르지 않게)
  if (const auto dash = s.rfind('-');
      dash != std::string::npos && dash > 0 && is_style_word(fold(s.substr(dash + 1)))) {
    s.erase(dash);
  }
  return fold(s);
}

std::vector<const FontEntry*> FontCatalog::match(std::string_view base_font_name,
                                                 bool allow_cff) const {
  const std::string want = normalize_name(base_font_name);
  std::vector<const FontEntry*> out;
  if (want.empty()) return out;
  for (const auto& e : entries_) {
    if (e.cff && !allow_cff) continue;
    const std::string family = normalize_name(e.family);
    // "ArialBold" 처럼 BaseFont 가 family+style 인 경우도 family 로 잡는다 (style 우선순위는 better
    // 가 정한다)
    if (family == want ||
        (want.size() > family.size() && want.compare(0, family.size(), family) == 0 &&
         want.substr(family.size()) == fold(e.style))) {
      out.push_back(&e);
    }
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const FontEntry* a, const FontEntry* b) { return better(*a, *b); });
  return out;
}

const FontEntry* FontCatalog::default_hangul_sans(bool allow_cff) const {
  static constexpr std::array<std::string_view, 8> kPreferred = {
      "Apple SD Gothic Neo", "Malgun Gothic", "맑은 고딕",   "Noto Sans CJK KR",
      "Noto Sans KR",        "NanumGothic",   "AppleGothic", "Gulim"};
  for (const auto name : kPreferred) {
    const auto found = match(name, allow_cff);
    if (!found.empty() && found.front()->hangul) return found.front();
  }
  const FontEntry* any = nullptr;
  for (const auto& e : entries_) {
    if (!e.hangul || (e.cff && !allow_cff)) continue;
    if (any == nullptr || better(e, *any)) any = &e;
  }
  return any;
}

}  // namespace onpdf::edit
