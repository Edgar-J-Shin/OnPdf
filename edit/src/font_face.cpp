#include "onpdf/edit/font_face.h"

#include "onpdf/core/platform/path.h"

#include <hb-ot.h>
#include <hb-subset.h>
#include <hb.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>

namespace onpdf::edit {
namespace {

constexpr float kWeight = 400.0f;  // 변형 폰트는 Regular 로 고정
constexpr hb_tag_t kWghtTag = HB_TAG('w', 'g', 'h', 't');

struct BlobDeleter { void operator()(hb_blob_t* b) const noexcept { hb_blob_destroy(b); } };
struct FaceDeleter { void operator()(hb_face_t* f) const noexcept { hb_face_destroy(f); } };
struct FontDeleter { void operator()(hb_font_t* f) const noexcept { hb_font_destroy(f); } };
struct BufferDeleter { void operator()(hb_buffer_t* b) const noexcept { hb_buffer_destroy(b); } };
struct SubsetInputDeleter { void operator()(hb_subset_input_t* i) const noexcept { hb_subset_input_destroy(i); } };
struct SubsetPlanDeleter { void operator()(hb_subset_plan_t* p) const noexcept { hb_subset_plan_destroy(p); } };
using Blob = std::unique_ptr<hb_blob_t, BlobDeleter>;
using Face = std::unique_ptr<hb_face_t, FaceDeleter>;
using Font = std::unique_ptr<hb_font_t, FontDeleter>;
using Buffer = std::unique_ptr<hb_buffer_t, BufferDeleter>;
using SubsetInput = std::unique_ptr<hb_subset_input_t, SubsetInputDeleter>;
using SubsetPlan = std::unique_ptr<hb_subset_plan_t, SubsetPlanDeleter>;

std::string fnv1a_hex(const std::vector<std::uint8_t>& bytes) {
  std::uint64_t h = 14695981039346656037ull;
  for (std::uint8_t b : bytes) {
    h ^= b;
    h *= 1099511628211ull;
  }
  char hex[17];
  std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
  return hex;
}

}  // namespace

bool is_layout_whitespace(char32_t c) noexcept { return c == U' ' || c == U'\t' || c == U'\n' || c == U'\r' || c == 0xA0; }

struct FontFace::Impl {
  std::vector<std::uint8_t> bytes;  // blob 이 참조한다 (선언 순서: bytes 가 blob 보다 오래 산다)
  Blob blob;
  Face face;
  bool has_wght = false;

  // 배치·측정용 폰트 (Regular 고정, 폰트 단위 = upem)
  Font make_font() const {
    Font font(hb_font_create(face.get()));
    const unsigned upem = hb_face_get_upem(face.get());
    hb_font_set_scale(font.get(), static_cast<int>(upem), static_cast<int>(upem));
    if (has_wght) {
      const hb_variation_t var{kWghtTag, kWeight};
      hb_font_set_variations(font.get(), &var, 1);
    }
    return font;
  }
};

FontFace::FontFace(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FontFace::~FontFace() = default;

Result<std::shared_ptr<const FontFace>> FontFace::load(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return make_error(ErrorCode::kFileNotFound, "cannot open font " + onpdf::path_to_utf8(path));
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return load_from_memory(std::move(bytes));
}

Result<std::shared_ptr<const FontFace>> FontFace::load_from_memory(std::vector<std::uint8_t> bytes) {
  if (bytes.empty()) return make_error(ErrorCode::kInvalidFormat, "empty font");
  auto impl = std::make_unique<Impl>();
  impl->bytes = std::move(bytes);
  impl->blob.reset(hb_blob_create(reinterpret_cast<const char*>(impl->bytes.data()), static_cast<unsigned>(impl->bytes.size()),
                                  HB_MEMORY_MODE_READONLY, nullptr, nullptr));
  if (!impl->blob || hb_blob_get_length(impl->blob.get()) == 0) return make_error(ErrorCode::kInvalidFormat, "cannot read font data");
  impl->face.reset(hb_face_create(impl->blob.get(), 0));
  if (!impl->face || hb_face_get_glyph_count(impl->face.get()) == 0) return make_error(ErrorCode::kInvalidFormat, "not a font");
  (void)hb_language_get_default();  // 첫 호출이 스레드 안전하지 않다 — 워커가 shape 에서 처음 부르기 전에 여기서(리뷰 I9)
  hb_ot_var_axis_info_t axis{};  // 출력 인자는 널이면 안 된다 (HarfBuzz 가 무조건 쓴다)
  impl->has_wght = hb_ot_var_find_axis_info(impl->face.get(), kWghtTag, &axis) != 0;
  return std::shared_ptr<const FontFace>(new FontFace(std::move(impl)));
}

unsigned FontFace::upem() const noexcept { return hb_face_get_upem(impl_->face.get()); }
bool FontFace::has_weight_axis() const noexcept { return impl_->has_wght; }

std::string FontFace::family_name() const {
  char buffer[128] = {};
  unsigned len = sizeof buffer;
  hb_ot_name_get_utf8(impl_->face.get(), HB_OT_NAME_ID_FONT_FAMILY, HB_LANGUAGE_INVALID, &len, buffer);
  return buffer;
}

ShapedText FontFace::shape(std::string_view utf8, double size_pt) const {
  ShapedText out;
  const Font font = impl_->make_font();
  const double scale = size_pt / static_cast<double>(upem());
  hb_font_extents_t extents{};
  if (hb_font_get_h_extents(font.get(), &extents)) {
    out.ascent = extents.ascender * scale;
    out.descent = -extents.descender * scale;
  } else {
    out.ascent = 0.88 * size_pt;
    out.descent = 0.12 * size_pt;
  }
  if (utf8.empty()) return out;
  Buffer buffer(hb_buffer_create());
  hb_buffer_add_utf8(buffer.get(), utf8.data(), static_cast<int>(utf8.size()), 0, static_cast<int>(utf8.size()));
  hb_buffer_guess_segment_properties(buffer.get());
  hb_buffer_set_cluster_level(buffer.get(), HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
  hb_shape(font.get(), buffer.get(), nullptr, 0);
  unsigned n = 0;
  const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer.get(), &n);
  const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer.get(), &n);
  // 클러스터(바이트 오프셋) 구간 → 입력 글자들. 다음 클러스터 시작은 자기보다 큰 가장 작은 오프셋 (방향 무관)
  std::vector<unsigned> starts;
  for (unsigned i = 0; i < n; ++i) starts.push_back(infos[i].cluster);
  std::sort(starts.begin(), starts.end());
  starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
  const auto text_of = [&](unsigned cluster) -> std::u32string {
    if (cluster >= utf8.size()) return U"\uFFFD";
    const auto next = std::upper_bound(starts.begin(), starts.end(), cluster);
    const unsigned end = next == starts.end() ? static_cast<unsigned>(utf8.size()) : *next;
    return decode_utf8(utf8.substr(cluster, end - cluster));
  };
  // 글리프 없는 레이아웃 공백(탭·NBSP 등)은 공백 글리프로 (리뷰 W1). 탭은 공백 4칸
  hb_codepoint_t space_gid = 0;
  const bool has_space = hb_font_get_nominal_glyph(font.get(), U' ', &space_gid) != 0;
  const double space_advance = has_space ? hb_font_get_glyph_h_advance(font.get(), space_gid) * scale : 0.25 * size_pt;
  out.glyphs.reserve(n);
  for (unsigned i = 0; i < n; ++i) {
    ShapedGlyph g;
    g.gid = infos[i].codepoint;
    g.text = text_of(infos[i].cluster);
    g.x_advance = positions[i].x_advance * scale;
    const bool blank = !g.text.empty() && std::all_of(g.text.begin(), g.text.end(), is_layout_whitespace);
    if (g.gid == 0 && blank && has_space) {
      g.gid = space_gid;
      g.x_advance = space_advance * (g.text.front() == U'\t' ? 4.0 : 1.0);
    }
    out.width += g.x_advance;
    out.glyphs.push_back(std::move(g));
  }
  return out;
}

std::u32string FontFace::missing(std::string_view utf8) const {
  const Font font = impl_->make_font();
  std::u32string out;
  std::set<char32_t> seen;
  for (char32_t c : decode_utf8(utf8)) {
    if (is_layout_whitespace(c) || seen.contains(c)) continue;
    seen.insert(c);
    hb_codepoint_t gid = 0;
    if (!hb_font_get_nominal_glyph(font.get(), c, &gid) || gid == 0) out.push_back(c);
  }
  return out;
}

Result<FontSubset> FontFace::subset(const std::vector<std::uint32_t>& gids) const {
  SubsetInput input(hb_subset_input_create_or_fail());
  if (!input) return make_error(ErrorCode::kInvalidState, "hb_subset_input_create failed");
  hb_set_t* glyph_set = hb_subset_input_glyph_set(input.get());
  hb_set_add(glyph_set, 0);  // .notdef
  for (std::uint32_t gid : gids) hb_set_add(glyph_set, gid);
  // GID 를 촘촘히 재배치(RETAIN_GIDS 아님) → CIDToGIDMap 이 작다. 힌팅은 버린다.
  // 레이아웃 클로저·GSUB/GPOS/GDEF 는 뺀다: PDF 뷰어는 임베딩 폰트를 재배치하지 않는다 (리뷰 I3, −25%)
  hb_subset_input_set_flags(input.get(), HB_SUBSET_FLAGS_NO_HINTING | HB_SUBSET_FLAGS_DESUBROUTINIZE | HB_SUBSET_FLAGS_NO_LAYOUT_CLOSURE);
  hb_set_t* drop = hb_subset_input_set(input.get(), HB_SUBSET_SETS_DROP_TABLE_TAG);
  for (hb_tag_t tag : {HB_TAG('G', 'S', 'U', 'B'), HB_TAG('G', 'P', 'O', 'S'), HB_TAG('G', 'D', 'E', 'F')}) hb_set_add(drop, tag);
  if (impl_->has_wght && !hb_subset_input_pin_axis_location(input.get(), impl_->face.get(), kWghtTag, kWeight)) {
    return make_error(ErrorCode::kInvalidState, "cannot pin weight axis");
  }
  SubsetPlan plan(hb_subset_plan_create_or_fail(impl_->face.get(), input.get()));
  if (!plan) return make_error(ErrorCode::kInvalidState, "hb_subset_plan_create failed");
  FontSubset out;
  const hb_map_t* mapping = hb_subset_plan_old_to_new_glyph_mapping(plan.get());
  for (std::uint32_t gid : gids) {
    const hb_codepoint_t mapped = hb_map_get(mapping, gid);
    if (mapped == HB_MAP_VALUE_INVALID) return make_error(ErrorCode::kInvalidState, "glyph dropped by subsetter");
    out.old_to_new[gid] = mapped;
  }
  Face face(hb_subset_plan_execute_or_fail(plan.get()));
  if (!face) return make_error(ErrorCode::kInvalidState, "hb_subset failed");
  Blob blob(hb_face_reference_blob(face.get()));
  unsigned length = 0;
  const char* data = hb_blob_get_data(blob.get(), &length);
  if (data == nullptr || length == 0) return make_error(ErrorCode::kInvalidState, "empty subset");
  out.ttf.assign(reinterpret_cast<const std::uint8_t*>(data), reinterpret_cast<const std::uint8_t*>(data) + length);
  out.glyph_count = hb_face_get_glyph_count(face.get());
  out.cid_to_gid.resize(static_cast<std::size_t>(out.glyph_count) * 2);
  for (unsigned i = 0; i < out.glyph_count; ++i) {
    out.cid_to_gid[static_cast<std::size_t>(i) * 2] = static_cast<std::uint8_t>(i >> 8);
    out.cid_to_gid[static_cast<std::size_t>(i) * 2 + 1] = static_cast<std::uint8_t>(i & 0xFF);
  }
  out.key = fnv1a_hex(out.ttf);
  return out;
}

std::string make_to_unicode_cmap(std::vector<std::pair<std::uint32_t, std::u32string>> cid_to_unicode) {
  std::stable_sort(cid_to_unicode.begin(), cid_to_unicode.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  // 같은 CID 는 먼저 나온 문자열만 (공백/NBSP 같은 공유 글리프: 입력에 나온 쪽)
  std::vector<std::pair<std::uint32_t, std::u32string>> unique;
  for (const auto& e : cid_to_unicode) {
    if (!unique.empty() && unique.back().first == e.first) continue;
    unique.push_back(e);
  }
  std::string s;
  s += "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n";
  s += "/CIDSystemInfo <</Registry (Adobe) /Ordering (Identity) /Supplement 0>> def\n";
  s += "/CMapName /OnPdf-Identity def\n/CMapType 2 def\n";
  s += "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
  for (std::size_t i = 0; i < unique.size(); i += 100) {  // bfchar 블록은 100개 이하 (PDF 규격)
    const std::size_t end = std::min(unique.size(), i + 100);
    s += std::to_string(end - i) + " beginbfchar\n";
    for (std::size_t k = i; k < end; ++k) {
      char head[16];
      std::snprintf(head, sizeof head, "<%04X> <", unique[k].first);
      s += head;
      for (const char32_t cp : unique[k].second) {  // UTF-16BE, 여러 코드 허용 (NFD·결합 문자·합자)
        char unit[12];
        if (cp >= 0x10000) {
          const char32_t v = cp - 0x10000;
          std::snprintf(unit, sizeof unit, "%04X%04X", 0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF));
        } else {
          std::snprintf(unit, sizeof unit, "%04X", static_cast<unsigned>(cp));
        }
        s += unit;
      }
      s += ">\n";
    }
    s += "endbfchar\n";
  }
  s += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
  return s;
}

std::u32string decode_utf8(std::string_view utf8) {
  std::u32string out;
  for (std::size_t i = 0; i < utf8.size();) {
    const auto c = static_cast<unsigned char>(utf8[i]);
    int n = 0;
    char32_t cp = 0;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
    else { out.push_back(U'�'); ++i; continue; }
    if (i + static_cast<std::size_t>(n) > utf8.size()) { out.push_back(U'�'); break; }
    bool ok = true;
    for (int k = 1; k < n; ++k) {
      const auto cc = static_cast<unsigned char>(utf8[i + static_cast<std::size_t>(k)]);
      if ((cc & 0xC0) != 0x80) { ok = false; break; }
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (!ok || (n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
      out.push_back(U'�');
      ++i;
      continue;
    }
    out.push_back(cp);
    i += static_cast<std::size_t>(n);
  }
  return out;
}

}  // namespace onpdf::edit
