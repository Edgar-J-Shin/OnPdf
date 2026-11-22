#include "text_lines.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>

namespace onpdf::engine::detail {
namespace {

using Mat = std::array<double, 6>;

// PDF 행 벡터 규약: p' = p × A × B
Mat mul(const Mat& A, const Mat& B) {
  return {A[0] * B[0] + A[1] * B[2],        A[0] * B[1] + A[1] * B[3],
          A[2] * B[0] + A[3] * B[2],        A[2] * B[1] + A[3] * B[3],
          A[4] * B[0] + A[5] * B[2] + B[4], A[4] * B[1] + A[5] * B[3] + B[5]};
}

void apply(const Mat& m, double x, double y, double& ox, double& oy) {
  ox = x * m[0] + y * m[2] + m[4];
  oy = x * m[1] + y * m[3] + m[5];
}

Mat matrix_of(FPDF_PAGEOBJECT obj) {
  FS_MATRIX m{};
  if (!FPDFPageObj_GetMatrix(obj, &m)) return {1, 0, 0, 1, 0, 0};
  return {m.a, m.b, m.c, m.d, m.e, m.f};
}

bool is_space(char32_t c) {
  return c == U' ' || c == 0xA0 || c == U'\t' || c == 0x3000;
}

std::u32string trim(const std::u32string& s) {
  std::size_t b = 0, e = s.size();
  while (b < e && is_space(s[b])) ++b;
  while (e > b && is_space(s[e - 1])) --e;
  return s.substr(b, e - b);
}

std::u32string utf16_to_u32(const std::vector<unsigned short>& w) {
  std::u32string out;
  for (std::size_t i = 0; i < w.size(); ++i) {
    char32_t c = w[i];
    if (c == 0) break;
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < w.size()) {
      c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<char32_t>(w[i + 1]) - 0xDC00);
      ++i;
    }
    out += c;
  }
  return out;
}

std::u32string object_text(FPDF_PAGEOBJECT obj, FPDF_TEXTPAGE tp) {
  const unsigned long bytes = FPDFTextObj_GetText(obj, tp, nullptr, 0);
  if (bytes == 0) return {};
  std::vector<unsigned short> buf(bytes / 2 + 1, 0);
  FPDFTextObj_GetText(obj, tp, reinterpret_cast<FPDF_WCHAR*>(buf.data()), bytes);
  return utf16_to_u32(buf);
}

// OnPdf 마크가 있는 객체인가 (이름 "OnPdf", UTF-16LE)
bool has_onpdf_mark(FPDF_PAGEOBJECT obj) {
  const int marks = FPDFPageObj_CountMarks(obj);
  for (int m = 0; m < marks; ++m) {
    FPDF_PAGEOBJECTMARK mark = FPDFPageObj_GetMark(obj, static_cast<unsigned long>(m));
    if (mark == nullptr) continue;
    unsigned short name[8] = {};
    unsigned long bytes = 0;
    if (!FPDFPageObjMark_GetName(mark, reinterpret_cast<FPDF_WCHAR*>(name), sizeof name, &bytes) ||
        bytes != 12)
      continue;
    if (name[0] == 'O' && name[1] == 'n' && name[2] == 'P' && name[3] == 'd' && name[4] == 'f')
      return true;
  }
  return false;
}

struct Collector {
  FPDF_TEXTPAGE tp = nullptr;
  PageAnalysis& out;
  int form_counter = 0;

  void visit(FPDF_PAGEOBJECT obj, int index, FPDF_PAGEOBJECT parent, int container,
             const Mat& container_matrix, int depth) {
    const int type = FPDFPageObj_GetType(obj);
    if (type == FPDF_PAGEOBJ_FORM) {
      const int my_container = depth == 0 ? form_counter++ : container;
      if (depth >= 1) {
        // 2단계 이상: 텍스트가 있는지만 본다 (편집 불가)
        if (has_text(obj, depth + 1)) out.has_deep_forms = true;
        return;
      }
      const Mat inner = mul(matrix_of(obj), container_matrix);
      const int n = FPDFFormObj_CountObjects(obj);
      for (int i = 0; i < n; ++i) {
        if (FPDF_PAGEOBJECT child = FPDFFormObj_GetObject(obj, static_cast<unsigned long>(i))) {
          visit(child, i, obj, my_container, inner, depth + 1);
        }
      }
      return;
    }
    if (type != FPDF_PAGEOBJ_TEXT) return;
    AnalyzedObject t;
    t.handle = obj;
    t.parent_form = parent;
    t.container = container;
    t.index = index;
    t.depth = depth;
    t.container_matrix = container_matrix;
    t.matrix = mul(matrix_of(obj), container_matrix);
    float l = 0, b = 0, r = 0, tp_ = 0;
    if (FPDFPageObj_GetBounds(obj, &l, &b, &r, &tp_)) {
      t.has_bounds = true;
      t.left = t.bottom = 1e18;
      t.right = t.top = -1e18;
      const double xs[2] = {l, r}, ys[2] = {b, tp_};
      for (double x : xs) {
        for (double y : ys) {
          double px = 0, py = 0;
          apply(container_matrix, x, y, px, py);
          t.left = std::min(t.left, px);
          t.right = std::max(t.right, px);
          t.bottom = std::min(t.bottom, py);
          t.top = std::max(t.top, py);
        }
      }
    }
    t.font = FPDFTextObj_GetFont(obj);
    FPDFTextObj_GetFontSize(obj, &t.size);
    const double hx = std::hypot(t.matrix[0], t.matrix[1]),
                 vy = std::hypot(t.matrix[2], t.matrix[3]);
    t.eff_size = t.size * (vy > 0 ? vy : 1.0);
    if (hx > 0) {
      t.dir_x = t.matrix[0] / hx;
      t.dir_y = t.matrix[1] / hx;
    }
    t.text = object_text(obj, tp);
    FPDFPageObj_GetFillColor(obj, &t.r, &t.g, &t.b, &t.a);
    t.render_mode = FPDFTextObj_GetTextRenderMode(obj);
    t.onpdf = has_onpdf_mark(obj);
    out.objects.push_back(std::move(t));
  }

  static bool has_text(FPDF_PAGEOBJECT form, int depth) {
    if (depth > 8) return false;
    const int n = FPDFFormObj_CountObjects(form);
    for (int i = 0; i < n; ++i) {
      FPDF_PAGEOBJECT child = FPDFFormObj_GetObject(form, static_cast<unsigned long>(i));
      if (child == nullptr) continue;
      const int type = FPDFPageObj_GetType(child);
      if (type == FPDF_PAGEOBJ_TEXT) return true;
      if (type == FPDF_PAGEOBJ_FORM && has_text(child, depth + 1)) return true;
    }
    return false;
  }
};

double along_of(const AnalyzedObject& o, double x, double y) {
  return x * o.dir_x + y * o.dir_y;
}
double normal_of(const AnalyzedObject& o) {
  return -o.matrix[4] * o.dir_y + o.matrix[5] * o.dir_x;
}

std::pair<double, double> along_range(const AnalyzedObject& o) {
  if (!o.has_bounds) {
    const double v = along_of(o, o.matrix[4], o.matrix[5]);
    return {v, v};
  }
  double lo = 1e18, hi = -1e18;
  const double xs[2] = {o.left, o.right}, ys[2] = {o.bottom, o.top};
  for (double x : xs) {
    for (double y : ys) {
      const double v = along_of(o, x, y);
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
  }
  return {lo, hi};
}

int dir_key(const AnalyzedObject& o) {
  return static_cast<int>(std::lround(std::atan2(o.dir_y, o.dir_x) * 180.0 / 3.14159265358979));
}

// 추출 글자 → 객체·줄 번호 대응
struct Extraction {
  std::map<FPDF_PAGEOBJECT, std::set<int>> obj_lines;
  std::vector<char32_t> chars;
  std::vector<FPDF_PAGEOBJECT> char_obj;
  std::vector<std::array<double, 4>>
      boxes;  // 글자 상자 l,b,r,t (쪽 사용자 좌표); 공백·생성 글자는 0
  std::vector<bool> has_box;
};

Extraction extract(FPDF_TEXTPAGE tp) {
  Extraction ex;
  const int n = FPDFText_CountChars(tp);
  int line = 0;
  bool line_has = false;
  for (int i = 0; i < n; ++i) {
    const char32_t c = static_cast<char32_t>(FPDFText_GetUnicode(tp, i));
    FPDF_PAGEOBJECT o = FPDFText_GetTextObject(tp, i);
    ex.chars.push_back(c);
    ex.char_obj.push_back(o);
    double l = 0, r = 0, b = 0, t = 0;
    const bool box = o != nullptr && !is_space(c) && c != U'\r' && c != U'\n' &&
                     FPDFText_GetCharBox(tp, i, &l, &r, &b, &t) && r > l;
    ex.boxes.push_back({box ? l : 0.0, box ? b : 0.0, box ? r : 0.0, box ? t : 0.0});
    ex.has_box.push_back(box);
    if (c == U'\r' || c == U'\n') {
      if (line_has) ++line;
      line_has = false;
      continue;
    }
    if (o != nullptr && !is_space(c)) {
      ex.obj_lines[o].insert(line);
      line_has = true;
    }
  }
  return ex;
}

// 줄 멤버 객체의 글자를 추출 순서대로 (객체 사이의 생성 공백 포함)
std::u32string line_text(const Extraction& ex, const std::set<FPDF_PAGEOBJECT>& members) {
  std::u32string out;
  bool pending_space = false;
  for (std::size_t i = 0; i < ex.chars.size(); ++i) {
    const char32_t c = ex.chars[i];
    if (c == U'\r' || c == U'\n') {
      if (!out.empty()) pending_space = true;
      continue;
    }
    if (ex.char_obj[i] != nullptr && members.count(ex.char_obj[i]) != 0) {
      if (pending_space && !out.empty() && !is_space(out.back()) && !is_space(c)) out += U' ';
      pending_space = false;
      out += c;
    } else if (!out.empty() && is_space(c)) {
      pending_space = true;
    }
  }
  return trim(out);
}

std::uint64_t fnv1a(const std::u32string& s) {
  std::uint64_t h = 14695981039346656037ULL;
  for (char32_t c : s) {
    for (int k = 0; k < 4; ++k) {
      h ^= static_cast<std::uint64_t>((c >> (8 * k)) & 0xFF);
      h *= 1099511628211ULL;
    }
  }
  return h;
}

struct GlyphSig {
  int segments = -1;
  std::vector<float> points;
  bool operator==(const GlyphSig& o) const { return segments == o.segments && points == o.points; }
};

GlyphSig glyph_signature(FPDF_FONT font, char32_t c) {
  GlyphSig s;
  FPDF_GLYPHPATH path = FPDFFont_GetGlyphPath(font, static_cast<uint32_t>(c), 12.0f);
  if (path == nullptr) return s;
  s.segments = FPDFGlyphPath_CountGlyphSegments(path);
  for (int i = 0; i < std::min(s.segments, 6); ++i) {
    float x = 0, y = 0;
    FPDF_PATHSEGMENT seg = FPDFGlyphPath_GetGlyphPathSegment(path, i);
    if (seg != nullptr && FPDFPathSegment_GetPoint(seg, &x, &y)) {
      s.points.push_back(x);
      s.points.push_back(y);
    }
  }
  return s;
}

}  // namespace

PageAnalysis analyze_page(FPDF_PAGE page, FPDF_TEXTPAGE text_page) {
  PageAnalysis out;
  Collector col{text_page, out};
  const int n = FPDFPage_CountObjects(page);
  for (int i = 0; i < n; ++i) {
    if (FPDF_PAGEOBJECT o = FPDFPage_GetObject(page, i))
      col.visit(o, i, nullptr, -1, {1, 0, 0, 1, 0, 0}, 0);
  }
  out.form_text_objects.assign(static_cast<std::size_t>(col.form_counter), 0);
  // 순번: 컨테이너 안 "비어 있지 않은" 텍스트 객체 순서 — qpdf 의 비어 있지 않은 표시 연산자 순번과
  // 같다 PDFium 은 비어 있지 않은 문자열이 하나라도 있어야 텍스트 객체를
  // 만들므로(Handle_ShowText/ShowText_Positioning) 모든 객체가 qpdf 의 "비어 있지 않은 표시 연산자"
  // 하나에 대응한다. 추출 글(FPDFTextObj_GetText)이 비는 공백만의 객체(` ( ) Tj`, Quartz· Word 가
  // 줄마다 수십 개)도 세야 한다 — 빼면 순번이 밀려 수술을 거절했다 (#75 측정: 코퍼스 65% 거절)
  for (auto& o : out.objects) {
    if (o.container < 0) {
      o.ordinal = out.page_text_objects++;
    } else {
      o.ordinal = out.form_text_objects[static_cast<std::size_t>(o.container)]++;
    }
  }
  const Extraction ex = extract(text_page);
  for (auto& o : out.objects) {
    if (auto it = ex.obj_lines.find(o.handle); it != ex.obj_lines.end())
      o.extract_lines.assign(it->second.begin(), it->second.end());
  }
  // 줄 묶기: 방향(1도) → 기준선 → 진행 방향 순 정렬 뒤, 같은 방향·기준선(±0.25em) 묶음을 1.5em 넘는
  // 간격에서 자른다
  std::vector<int> idx;
  for (int i = 0; i < static_cast<int>(out.objects.size()); ++i) {
    const auto& o = out.objects[static_cast<std::size_t>(i)];
    if (!o.onpdf && !trim(o.text).empty() && o.eff_size > 0.1)
      idx.push_back(i);  // OnPdf 상자는 줄이 아니다
  }
  auto obj = [&](int i) -> const AnalyzedObject& {
    return out.objects[static_cast<std::size_t>(i)];
  };
  std::sort(idx.begin(), idx.end(), [&](int x, int y) {
    const int dx = dir_key(obj(x)), dy = dir_key(obj(y));
    if (dx != dy) return dx < dy;
    if (obj(x).container != obj(y).container) return obj(x).container < obj(y).container;
    const double nx = normal_of(obj(x)), ny = normal_of(obj(y));
    if (std::abs(nx - ny) > 1e-6) return nx > ny;
    return along_range(obj(x)).first < along_range(obj(y)).first;
  });
  std::vector<std::vector<int>> bands;
  for (int i : idx) {
    const auto& o = obj(i);
    if (!bands.empty()) {
      const auto& last = obj(bands.back().front());
      if (dir_key(last) == dir_key(o) && last.container == o.container &&
          std::abs(normal_of(last) - normal_of(o)) <= 0.25 * std::max(last.eff_size, o.eff_size)) {
        bands.back().push_back(i);
        continue;
      }
    }
    bands.push_back({i});
  }
  for (auto& band : bands) {
    std::sort(band.begin(), band.end(),
              [&](int x, int y) { return along_range(obj(x)).first < along_range(obj(y)).first; });
    AnalyzedLine cur;
    double cur_end = -1e18;
    auto flush = [&] {
      if (cur.members.empty()) return;
      std::set<FPDF_PAGEOBJECT> members;
      std::set<int> extract_ids;
      FPDF_FONT first_font = nullptr;
      float first_size = 0;
      for (int k : cur.members) {
        const auto& o = obj(k);
        members.insert(o.handle);
        extract_ids.insert(o.extract_lines.begin(), o.extract_lines.end());
        if (o.extract_lines.size() > 1) cur.multi_line_object = true;
        if (first_font == nullptr) {
          first_font = o.font;
          first_size = o.size;
        } else if (o.font != first_font || std::abs(o.size - first_size) > 0.01f) {
          cur.mixed_fonts = true;
        }
      }
      cur.merged_lines = extract_ids.size() > 1;
      for (std::size_t i = 0; i < ex.chars.size();
           ++i) {  // 잉크 폭: 멤버 객체의 비공백 글자 상자 (#77)
        if (!ex.has_box[i] || !members.count(ex.char_obj[i])) continue;
        cur.ink_left = std::min(cur.ink_left, ex.boxes[i][0]);
        cur.ink_right = std::max(cur.ink_right, ex.boxes[i][2]);
        ++cur.ink_chars;
      }
      cur.invisible =
          obj(cur.members.front()).render_mode == 3 || obj(cur.members.front()).render_mode == 7;
      cur.text = line_text(ex, members);
      cur.container = obj(cur.members.front()).container;
      out.lines.push_back(std::move(cur));
      cur = AnalyzedLine{};
      cur_end = -1e18;
    };
    for (int i : band) {
      const auto& o = obj(i);
      const auto [lo, hi] = along_range(o);
      if (!cur.members.empty() && lo - cur_end > 1.5 * std::max(cur.eff_size, o.eff_size)) flush();
      if (cur.members.empty()) {
        cur.dir_x = o.dir_x;
        cur.dir_y = o.dir_y;
        cur.left = cur.bottom = 1e18;
        cur.right = cur.top = -1e18;
      }
      cur.members.push_back(i);
      cur.eff_size = std::max(cur.eff_size, o.eff_size);
      if (o.has_bounds) {
        cur.left = std::min(cur.left, o.left);
        cur.right = std::max(cur.right, o.right);
        cur.bottom = std::min(cur.bottom, o.bottom);
        cur.top = std::max(cur.top, o.top);
      }
      cur_end = std::max(cur_end, hi);
    }
    flush();
  }
  return out;
}

std::string line_id_of(const PageAnalysis& analysis, const AnalyzedLine& line) {
  const auto& first = analysis.objects[static_cast<std::size_t>(line.members.front())];
  std::string id = first.container < 0 ? "p" : "f" + std::to_string(first.container);
  id += "/" + std::to_string(first.index) + "/";
  char hex[17];
  std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(fnv1a(line.text)));
  return id + hex;
}

bool font_covers(FPDF_FONT font, const std::u32string& chars) {
  if (font == nullptr) return false;
  // 없는 글자의 서명(.notdef): 사용자 정의 영역(U+E000)과 cmap 에 거의 없는 U+10FFFD 두 곳을 찍는다
  // — PUA 에 진짜 글리프가 있는 글꼴(기호·옛 한글)에서도 하나는 .notdef 가 된다 (리뷰 I4)
  const GlyphSig probes[2] = {glyph_signature(font, 0xE000), glyph_signature(font, 0x10FFFD)};
  for (char32_t c : chars) {
    if (is_space(c) || c == U'\n' || c == U'\r') continue;
    const GlyphSig s = glyph_signature(font, c);
    if (s.segments <= 0) return false;
    for (const GlyphSig& notdef : probes) {
      if (notdef.segments > 0 && s == notdef) return false;
    }
  }
  return true;
}

std::string utf32_to_utf8(const std::u32string& s) {
  std::string out;
  for (char32_t cp : s) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  return out;
}

std::u32string utf8_to_utf32(std::string_view s) {
  std::u32string out;
  for (std::size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    char32_t cp = 0;
    std::size_t n = 1;
    if (c < 0x80) {
      cp = c;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      n = 2;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      n = 3;
    } else {
      cp = c & 0x07;
      n = 4;
    }
    for (std::size_t k = 1; k < n && i + k < s.size(); ++k)
      cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    i += n;
    out += cp;
  }
  return out;
}

}  // namespace onpdf::engine::detail
