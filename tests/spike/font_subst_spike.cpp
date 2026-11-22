// Spike #71: M4 폰트 대체 조건 — (1) OTF(CFF) 서브셋 산출물의 qpdf 후처리(/FontFile3 /OpenType +
// CIDFontType0) (2) TTC 면 선택 (3) 시스템 글꼴 후보 탐색. 제품 코드가 아니다 — qpdf·HarfBuzz
// 헤더를 직접 쓴다(스파이크 예외).
//
// 실행: ONPDF_SPIKE_FONT_DIR=<NotoSerifCJKkr-Regular.otf·NotoSansCJK-Regular.ttc 가 있는 폴더>
// onpdf_spike_font_subst
#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/qpdf_tools.h"
#include "onpdf/edit/add_text_command.h"
#include "onpdf/edit/font_face.h"

#include "../test_support.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <hb-ot.h>
#include <hb-subset.h>
#include <hb.h>
#include <iostream>
#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFWriter.hh>
#include <string>
#include <vector>

namespace onpdf {
namespace {

using engine::PdfiumDocument;
using onpdf::Bitmap;
using onpdf::test::read_bytes;
using onpdf::test::ScopedTempDir;
using onpdf::test::testdata;

std::filesystem::path font_dir() {
  const char* env = std::getenv("ONPDF_SPIKE_FONT_DIR");
  return env ? std::filesystem::path(env) : std::filesystem::path{};
}

void write_bytes(const std::filesystem::path& p, const std::vector<std::uint8_t>& b) {
  std::ofstream o(p, std::ios::binary);
  o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

std::filesystem::path out_dir() {
  const auto d = std::filesystem::temp_directory_path() / "onpdf-spike71";
  std::filesystem::create_directories(d);
  return d;
}

long dark_pixels(const Bitmap& b) {
  long n = 0;
  for (int y = 0; y < b.height; ++y)
    for (int x = 0; x < b.width; ++x) {
      const auto* p = b.row(y) + x * 4;
      if (p[0] < 128 && p[1] < 128 && p[2] < 128) ++n;
    }
  return n;
}

long diff_pixels(const Bitmap& a, const Bitmap& b) {
  if (a.width != b.width || a.height != b.height) return -1;
  long n = 0;
  for (int y = 0; y < a.height; ++y) {
    const auto* pa = a.row(y);
    const auto* pb = b.row(y);
    for (int x = 0; x < a.width * 4; x += 4)
      if (pa[x] != pb[x] || pa[x + 1] != pb[x + 1] || pa[x + 2] != pb[x + 2]) ++n;
  }
  return n;
}

edit::TextBoxSpec box(std::shared_ptr<const edit::FontFace> face, const std::string& text,
                      double size = 24.0) {
  edit::TextBoxSpec s;
  s.page = 0;
  s.origin = {72, 100};
  s.size = size;
  s.color = {0, 0, 0, 255};
  s.utf8 = text;
  s.face = std::move(face);
  return s;
}

// 글꼴을 simple.pdf 에 넣고 저장한 바이트
std::vector<std::uint8_t> embed(const std::shared_ptr<const edit::FontFace>& face,
                                const std::string& text) {
  ScopedTempDir dir;
  auto doc = PdfiumDocument::open_from_memory(read_bytes(testdata("simple.pdf")));
  if (!doc) return {};
  edit::AddTextCommand cmd(box(face, text));
  auto exec = cmd.execute(*doc);
  if (!exec) {
    std::cout << "  execute failed: " << exec.error().message << "\n";
    return {};
  }
  const auto p = dir / "embedded.pdf";
  if (!doc->save_to(p, false)) return {};
  return read_bytes(p);
}

// ---------- (1) CFF 후처리 ----------

struct CffFix {
  int fixed = 0;
  std::string tag;
  std::size_t font_bytes = 0;
  bool cid_keyed = false;
  bool charset_identity_before = false;
  bool charset_rewritten = false;
  unsigned glyphs = 0;
};

// ---- 최소 CFF 파서: OTTO 안의 CFF 테이블에서 Top DICT 의 ROS·charset·CharStrings 를 읽고 charset
// 을 항등으로 고쳐 쓴다 ---- 근거: hb-subset 은 CID-keyed CFF 의 charset 에 **원래 CID** 를
// 남긴다(GID 재배치와 무관). CIDFontType0 에서 CoreGraphics·Adobe 는 CID→charset 으로 글리프를 찾고
// PDFium(FreeType, SFNT 포장)은 CID=GID 로 찾으므로, 우리 코드(=새 GID)가 둘 다에서 맞으려면
// charset 이 항등이어야 한다. charset 은 크기 필드가 없고 다음 구조는 오프셋으로 찾으므로 같은
// 자리에 더 짧은 항등 charset 을 덮어써도 된다
struct CffView {
  std::vector<std::uint8_t>* otf = nullptr;
  std::size_t cff = 0;  // CFF 테이블 시작
  bool cid_keyed = false;
  std::size_t charset_off = 0;  // CFF 기준
  unsigned glyphs = 0;
  std::size_t charset_len = 0;
  bool identity = false;
};

std::uint32_t be(const std::vector<std::uint8_t>& b, std::size_t off, int n) {
  std::uint32_t v = 0;
  for (int i = 0; i < n; ++i) v = (v << 8) | b[off + static_cast<std::size_t>(i)];
  return v;
}

// INDEX 를 건너뛰고 항목 범위를 돌려준다
std::vector<std::pair<std::size_t, std::size_t>> cff_index(const std::vector<std::uint8_t>& b,
                                                           std::size_t& off) {
  std::vector<std::pair<std::size_t, std::size_t>> items;
  const unsigned count = be(b, off, 2);
  if (count == 0) {
    off += 2;
    return items;
  }
  const int osz = b[off + 2];
  std::size_t o = off + 3;
  std::vector<std::uint32_t> offs;
  for (unsigned i = 0; i <= count; ++i)
    offs.push_back(be(b, o + i * static_cast<std::size_t>(osz), osz));
  const std::size_t base = o + (count + 1) * static_cast<std::size_t>(osz) - 1;
  for (unsigned i = 0; i < count; ++i) items.emplace_back(base + offs[i], base + offs[i + 1]);
  off = base + offs[count];
  return items;
}

// DICT 에서 op 의 첫 정수 피연산자 (없으면 -1)
long cff_dict_int(const std::vector<std::uint8_t>& b, std::size_t begin, std::size_t end,
                  int want_op, bool* present = nullptr) {
  std::vector<long> ops;
  for (std::size_t i = begin; i < end;) {
    const int b0 = b[i];
    if (b0 <= 21) {
      int op = b0;
      ++i;
      if (b0 == 12) {
        op = 1200 + b[i];
        ++i;
      }
      if (op == want_op) {
        if (present) *present = true;
        return ops.empty() ? -1 : ops[0];
      }
      ops.clear();
    } else if (b0 == 28) {
      ops.push_back(static_cast<std::int16_t>(be(b, i + 1, 2)));
      i += 3;
    } else if (b0 == 29) {
      ops.push_back(static_cast<std::int32_t>(be(b, i + 1, 4)));
      i += 5;
    } else if (b0 == 30) {
      ++i;
      while (true) {
        const int v = b[i++];
        if ((v & 0x0f) == 0x0f || (v >> 4) == 0x0f) break;
      }
      ops.push_back(0);
    } else if (b0 >= 32 && b0 <= 246) {
      ops.push_back(b0 - 139);
      ++i;
    } else if (b0 >= 247 && b0 <= 250) {
      ops.push_back((b0 - 247) * 256 + b[i + 1] + 108);
      i += 2;
    } else if (b0 >= 251 && b0 <= 254) {
      ops.push_back(-(b0 - 251) * 256 - b[i + 1] - 108);
      i += 2;
    } else
      ++i;
  }
  return -1;
}

bool cff_view(std::vector<std::uint8_t>& otf, CffView& v) {
  v.otf = &otf;
  const unsigned num = be(otf, 4, 2);
  for (unsigned i = 0; i < num; ++i) {
    const std::size_t rec = 12 + i * 16;
    if (std::string(reinterpret_cast<const char*>(&otf[rec]), 4) == "CFF ")
      v.cff = be(otf, rec + 8, 4);
  }
  if (v.cff == 0) return false;
  std::size_t off = v.cff + otf[v.cff + 2];
  (void)cff_index(otf, off);       // Name INDEX
  auto top = cff_index(otf, off);  // Top DICT INDEX
  if (top.empty()) return false;
  bool has_ros = false;
  (void)cff_dict_int(otf, top[0].first, top[0].second, 1230, &has_ros);
  v.cid_keyed = has_ros;
  const long charset = cff_dict_int(otf, top[0].first, top[0].second, 15);
  const long charstrings = cff_dict_int(otf, top[0].first, top[0].second, 17);
  if (charset <= 2 || charstrings <= 0)
    return false;  // 0·1·2 는 사전 정의 charset (CID 글꼴에는 없다)
  std::size_t cs = v.cff + static_cast<std::size_t>(charstrings);
  v.glyphs = be(otf, cs, 2);
  v.charset_off = v.cff + static_cast<std::size_t>(charset);
  // 기존 charset 길이·항등 여부
  const int fmt = otf[v.charset_off];
  std::size_t i = v.charset_off + 1;
  unsigned gid = 1;
  v.identity = true;
  if (fmt == 0) {
    for (; gid < v.glyphs; ++gid, i += 2)
      if (be(otf, i, 2) != gid) v.identity = false;
  } else {
    while (gid < v.glyphs) {
      const unsigned first = be(otf, i, 2);
      const unsigned left = fmt == 1 ? be(otf, i + 2, 1) : be(otf, i + 2, 2);
      i += fmt == 1 ? 3 : 4;
      for (unsigned k = 0; k <= left && gid < v.glyphs; ++k, ++gid)
        if (first + k != gid) v.identity = false;
    }
  }
  v.charset_len = i - v.charset_off;
  return true;
}

// charset 을 항등으로 덮어쓴다 (같은 자리, 더 짧거나 같은 길이)
bool cff_write_identity_charset(CffView& v) {
  auto& b = *v.otf;
  const unsigned n = v.glyphs;
  if (n <= 1) return true;
  std::vector<std::uint8_t> out;
  if (n - 2 <= 255 && v.charset_len >= 4)
    out = {1, 0, 1, static_cast<std::uint8_t>(n - 2)};  // 형식 1: first=1, nLeft=n-2
  else if (v.charset_len >= 5)
    out = {2, 0, 1, static_cast<std::uint8_t>((n - 2) >> 8),
           static_cast<std::uint8_t>((n - 2) & 0xFF)};
  else if (v.charset_len >= 1 + 2 * (n - 1)) {  // 형식 0
    out.push_back(0);
    for (unsigned g = 1; g < n; ++g) {
      out.push_back(static_cast<std::uint8_t>(g >> 8));
      out.push_back(static_cast<std::uint8_t>(g & 0xFF));
    }
  } else
    return false;
  std::copy(out.begin(), out.end(), b.begin() + static_cast<std::ptrdiff_t>(v.charset_off));
  return true;
}

// /FontFile2 의 데이터가 OTTO 면: FontFile2 → FontFile3(/Subtype /OpenType), CIDFontType2 →
// CIDFontType0, CIDToGIDMap 제거
CffFix cff_postprocess(const std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& out,
                       bool identity_charset) {
  CffFix fix;
  QPDF q;
  q.processMemoryFile("in.pdf", reinterpret_cast<const char*>(in.data()), in.size());
  for (auto& oh : q.getAllObjects()) {
    if (!oh.isDictionaryOfType("/Font") || !oh.getKey("/Subtype").isNameAndEquals("/CIDFontType2"))
      continue;
    QPDFObjectHandle desc = oh.getKey("/FontDescriptor");
    if (!desc.isDictionary() || !desc.hasKey("/FontFile2")) continue;
    QPDFObjectHandle file = desc.getKey("/FontFile2");
    auto data = file.getStreamData();
    if (data->getSize() < 4) continue;
    fix.tag.assign(reinterpret_cast<const char*>(data->getBuffer()), 4);
    if (fix.tag != "OTTO") continue;
    fix.font_bytes = data->getSize();
    std::vector<std::uint8_t> otf(data->getBuffer(), data->getBuffer() + data->getSize());
    CffView view;
    if (cff_view(otf, view)) {
      fix.cid_keyed = view.cid_keyed;
      fix.charset_identity_before = view.identity;
      fix.glyphs = view.glyphs;
      if (view.cid_keyed && !view.identity && identity_charset) {
        fix.charset_rewritten = cff_write_identity_charset(view);
        file.replaceStreamData(std::string(otf.begin(), otf.end()), QPDFObjectHandle::newNull(),
                               QPDFObjectHandle::newNull());
      }
    }
    file.getDict().replaceKey("/Subtype", QPDFObjectHandle::newName("/OpenType"));
    desc.removeKey("/FontFile2");
    desc.replaceKey("/FontFile3", file);
    oh.replaceKey("/Subtype", QPDFObjectHandle::newName("/CIDFontType0"));
    oh.removeKey("/CIDToGIDMap");
    ++fix.fixed;
  }
  QPDFWriter w(q);
  w.setOutputMemory();
  w.setObjectStreamMode(qpdf_o_preserve);
  w.write();
  auto buf = w.getBufferSharedPointer();
  out.assign(buf->getBuffer(), buf->getBuffer() + buf->getSize());
  return fix;
}

void verify_cff_postprocess(const std::filesystem::path& font_path, const std::string& text,
                            const char* note, bool identity_charset) {
  auto face = edit::FontFace::load(font_path);
  if (!face) GTEST_SKIP() << "font not loadable: " << font_path;
  const auto before = embed(*face, text);
  ASSERT_FALSE(before.empty());
  std::vector<std::uint8_t> after;
  const auto t0 = std::chrono::steady_clock::now();
  const CffFix fix = cff_postprocess(before, after, identity_charset);
  const auto ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::cout << "[cff] " << note << " tag=" << fix.tag << " fixed=" << fix.fixed
            << " font_bytes=" << fix.font_bytes << " cid_keyed=" << fix.cid_keyed
            << " glyphs=" << fix.glyphs
            << " charset_identity_before=" << fix.charset_identity_before
            << " rewritten=" << fix.charset_rewritten << " size " << before.size() << " -> "
            << after.size() << " (" << ms << " ms)\n";
  ASSERT_EQ(fix.fixed, 1);
  const auto dir = out_dir();
  const auto p_before = dir / (std::string(note) + "-fontfile2.pdf");
  const auto p_after = dir / (std::string(note) + "-fontfile3.pdf");
  write_bytes(p_before, before);
  write_bytes(p_after, after);
  // 서브셋 CFF 를 떨어뜨려 Python 으로 charset(CID 매핑)을 본다
  {
    QPDF q;
    q.processMemoryFile("in.pdf", reinterpret_cast<const char*>(after.data()), after.size());
    for (auto& oh : q.getAllObjects()) {
      if (oh.isDictionary() && oh.hasKey("/FontFile3")) {
        auto data = oh.getKey("/FontFile3").getStreamData();
        std::vector<std::uint8_t> bytes(data->getBuffer(), data->getBuffer() + data->getSize());
        write_bytes(dir / (std::string(note) + "-subset.otf"), bytes);
      }
    }
  }
  auto check = engine::QpdfTools::check(p_after);
  ASSERT_TRUE(check) << check.error().message;
  std::cout << "[cff]   qpdf check warnings=" << check->warnings.size() << "\n";
  auto a = PdfiumDocument::open(p_before);
  auto b = PdfiumDocument::open(p_after);
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  engine::RenderOptions ro;
  ro.scale = 2.0f;
  auto ra = a->render_page(0, ro);
  auto rb = b->render_page(0, ro);
  ASSERT_TRUE(ra);
  ASSERT_TRUE(rb);
  const long diff = diff_pixels(*ra, *rb);
  auto ta = a->load_text(0);
  auto tb = b->load_text(0);
  ASSERT_TRUE(ta);
  ASSERT_TRUE(tb);
  const bool found = tb->all_text().find(text) != std::string::npos;
  std::cout << "[cff]   PDFium render diff=" << diff << " px (dark " << dark_pixels(*ra) << " -> "
            << dark_pixels(*rb)
            << ") extract same=" << (ta->all_text() == tb->all_text() ? "yes" : "NO")
            << " found=" << (found ? "yes" : "NO") << "\n";
  std::cout << "[cff]   files: " << p_before << " / " << p_after << "\n";
  EXPECT_EQ(diff, 0) << "CIDFontType0 로 바꿔도 PDFium 은 같은 글리프를 그려야 한다";
  EXPECT_TRUE(found);
}

TEST(FontSubstSpike, CffSerifPostprocessRawCharset) {  // 사전만 고침 → CoreGraphics 가 그리지
                                                       // 않는다 (charset = 원래 CID)
  if (font_dir().empty()) GTEST_SKIP();
  verify_cff_postprocess(font_dir() / "NotoSerifCJKkr-Regular.otf", "명조 OTF 한글 Serif 123",
                         "cff-serif-raw", false);
}
TEST(FontSubstSpike, CffSerifPostprocess) {
  if (font_dir().empty()) GTEST_SKIP();
  verify_cff_postprocess(font_dir() / "NotoSerifCJKkr-Regular.otf", "명조 OTF 한글 Serif 123",
                         "cff-serif", true);
}
TEST(FontSubstSpike, CffSansPostprocess) {
  if (font_dir().empty()) GTEST_SKIP();
  verify_cff_postprocess(font_dir() / "NotoSansCJKkr-Regular.otf", "고딕 OTF 한글 Sans 123",
                         "cff-sans", true);
}
TEST(FontSubstSpike,
     CffAppleSdGothicPostprocess) {  // 시스템 CFF TTC (면 0) — charset 이 항등이 아닌 글꼴
  verify_cff_postprocess("/System/Library/Fonts/AppleSDGothicNeo.ttc", "애플 SD 고딕 Neo 한글 123",
                         "cff-apple", true);
}
TEST(FontSubstSpike, GlyfIsUntouched) {
  auto face = edit::FontFace::load(testdata("fonts") / "NotoSansKR-subset.ttf");
  ASSERT_TRUE(face);
  const auto before = embed(*face, "한글 텍스트");
  ASSERT_FALSE(before.empty());
  std::vector<std::uint8_t> after;
  const CffFix fix = cff_postprocess(before, after, true);
  std::cout << "[cff] glyf tag=" << fix.tag << " fixed=" << fix.fixed << "\n";
  EXPECT_EQ(fix.fixed, 0);
}

// ---------- (2) TTC 면 선택 ----------

struct HbBlobDeleter {
  void operator()(hb_blob_t* b) const noexcept { hb_blob_destroy(b); }
};
struct HbFaceDeleter {
  void operator()(hb_face_t* f) const noexcept { hb_face_destroy(f); }
};
struct HbFontDeleter {
  void operator()(hb_font_t* f) const noexcept { hb_font_destroy(f); }
};
using HbBlob = std::unique_ptr<hb_blob_t, HbBlobDeleter>;
using HbFace = std::unique_ptr<hb_face_t, HbFaceDeleter>;
using HbFont = std::unique_ptr<hb_font_t, HbFontDeleter>;

std::string name_of(hb_face_t* face, hb_ot_name_id_t id) {
  char buffer[256] = {};
  unsigned len = sizeof buffer;
  hb_ot_name_get_utf8(face, id, HB_LANGUAGE_INVALID, &len, buffer);
  return buffer;
}

// TTC 의 n 번째 면을 독립 글꼴(모든 글리프 유지)로 — 제품에서는 FontFace::load(path, index) 가
// hb_face_create(blob, index) 로 대신한다
std::vector<std::uint8_t> extract_face(hb_blob_t* blob, unsigned index) {
  HbFace face(hb_face_create(blob, index));
  hb_subset_input_t* input = hb_subset_input_create_or_fail();
  hb_set_add_range(hb_subset_input_glyph_set(input), 0, hb_face_get_glyph_count(face.get()) - 1);
  hb_subset_input_set_flags(input, HB_SUBSET_FLAGS_RETAIN_GIDS | HB_SUBSET_FLAGS_NO_LAYOUT_CLOSURE);
  HbFace out(hb_subset_or_fail(face.get(), input));
  hb_subset_input_destroy(input);
  if (!out) return {};
  HbBlob ob(hb_face_reference_blob(out.get()));
  unsigned len = 0;
  const char* data = hb_blob_get_data(ob.get(), &len);
  return {reinterpret_cast<const std::uint8_t*>(data),
          reinterpret_cast<const std::uint8_t*>(data) + len};
}

TEST(FontSubstSpike, TtcFaceSelection) {
  if (font_dir().empty()) GTEST_SKIP();
  const auto path = font_dir() / "NotoSansCJK-Regular.ttc";
  const auto bytes = read_bytes(path);
  if (bytes.empty()) GTEST_SKIP();
  HbBlob blob(hb_blob_create(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<unsigned>(bytes.size()), HB_MEMORY_MODE_READONLY, nullptr,
                             nullptr));
  const unsigned count = hb_face_count(blob.get());
  std::cout << "[ttc] faces=" << count << "\n";
  int kr_index = -1;
  for (unsigned i = 0; i < count; ++i) {
    HbFace f(hb_face_create(blob.get(), i));
    const std::string family = name_of(f.get(), HB_OT_NAME_ID_FONT_FAMILY);
    const std::string full = name_of(f.get(), HB_OT_NAME_ID_FULL_NAME);
    HbFont font(hb_font_create(f.get()));
    hb_codepoint_t gid = 0;
    const bool has_ga = hb_font_get_nominal_glyph(font.get(), U'가', &gid);
    std::cout << "[ttc]   " << i << ": " << family << " / " << full
              << " glyphs=" << hb_face_get_glyph_count(f.get())
              << " 가=" << (has_ga ? std::to_string(gid) : "none") << "\n";
    if (kr_index < 0 && family.find("KR") != std::string::npos) kr_index = static_cast<int>(i);
  }
  ASSERT_GE(kr_index, 0) << "KR 면이 있어야 한다";
  // JP 와 KR 면의 cmap 이 다른 한자를 찾아 본문에 넣는다 (같은 글리프 집합, 지역 자형만 다르다)
  std::string text = "TTC 면 선택 한글 ";
  {
    HbFace jp(hb_face_create(blob.get(), 0));
    HbFace kr(hb_face_create(blob.get(), static_cast<unsigned>(kr_index)));
    HbFont fj(hb_font_create(jp.get()));
    HbFont fk(hb_font_create(kr.get()));
    int differ = 0;
    for (char32_t cp = 0x4E00; cp <= 0x9FFF; ++cp) {
      hb_codepoint_t a = 0, b = 0;
      if (!hb_font_get_nominal_glyph(fj.get(), cp, &a) ||
          !hb_font_get_nominal_glyph(fk.get(), cp, &b) || a == b)
        continue;
      if (differ < 4) {
        char buf[5] = {};
        if (cp < 0x10000) {
          buf[0] = static_cast<char>(0xE0 | (cp >> 12));
          buf[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
          buf[2] = static_cast<char>(0x80 | (cp & 0x3F));
        }
        text += buf;
      }
      ++differ;
    }
    std::cout << "[ttc]   JP/KR cmap differs for " << differ << " CJK ideographs; text=" << text
              << "\n";
  }
  std::vector<Bitmap> renders;
  for (const unsigned index : {0u, static_cast<unsigned>(kr_index)}) {
    const auto t0 = std::chrono::steady_clock::now();
    auto standalone = extract_face(blob.get(), index);
    const auto ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_FALSE(standalone.empty());
    auto face = edit::FontFace::load_from_memory(std::move(standalone));
    ASSERT_TRUE(face) << face.error().message;
    std::cout << "[ttc]   face " << index << " extracted in " << ms
              << " ms family=" << (*face)->family_name()
              << " missing=" << (*face)->missing(text).size() << "\n";
    const auto pdf = embed(*face, text);
    ASSERT_FALSE(pdf.empty());
    const auto p = out_dir() / ("ttc-face" + std::to_string(index) + ".pdf");
    write_bytes(p, pdf);
    auto doc = PdfiumDocument::open(p);
    ASSERT_TRUE(doc);
    engine::RenderOptions ro;
    ro.scale = 2.0f;
    auto bmp = doc->render_page(0, ro);
    ASSERT_TRUE(bmp);
    auto extracted = doc->load_text(0);
    ASSERT_TRUE(extracted);
    std::cout << "[ttc]   face " << index << " pdf=" << pdf.size() << " dark=" << dark_pixels(*bmp)
              << " extracted="
              << (extracted->all_text().find(text) != std::string::npos ? "yes" : "NO") << " -> "
              << p << "\n";
    renders.push_back(std::move(*bmp));
  }
  std::cout << "[ttc]   JP vs KR render diff=" << diff_pixels(renders[0], renders[1])
            << " px (한자 자형이 다르면 0 이 아니다)\n";
}

// ---------- (3) 시스템 글꼴 탐색 ----------

struct Candidate {
  std::filesystem::path path;
  unsigned index;
  std::string family;
  std::string style;
  bool cff;
  bool has_hangul;
  unsigned glyphs;
};

TEST(FontSubstSpike, SystemFontScan) {
  std::vector<std::filesystem::path> dirs;
#ifdef __APPLE__
  dirs = {"/System/Library/Fonts", "/System/Library/Fonts/Supplemental", "/Library/Fonts"};
  if (const char* home = std::getenv("HOME"))
    dirs.emplace_back(std::filesystem::path(home) / "Library/Fonts");
#elif defined(_WIN32)
  dirs = {"C:/Windows/Fonts"};
#else
  dirs = {"/usr/share/fonts"};
#endif
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<Candidate> all;
  int files = 0;
  for (const auto& d : dirs) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(d, ec), end; it != end; it.increment(ec)) {
      if (ec || !it->is_regular_file(ec)) continue;
      std::string ext = it->path().extension().string();
      for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      if (ext != ".ttf" && ext != ".otf" && ext != ".ttc") continue;
      ++files;
      HbBlob blob(hb_blob_create_from_file_or_fail(it->path().string().c_str()));
      if (!blob) continue;
      const unsigned count = hb_face_count(blob.get());
      for (unsigned i = 0; i < count; ++i) {
        HbFace face(hb_face_create(blob.get(), i));
        if (hb_face_get_glyph_count(face.get()) == 0) continue;
        HbFont font(hb_font_create(face.get()));
        hb_codepoint_t gid = 0;
        const bool hangul = hb_font_get_nominal_glyph(font.get(), U'한', &gid) &&
                            hb_font_get_nominal_glyph(font.get(), U'글', &gid);
        HbBlob cff(hb_face_reference_table(face.get(), HB_TAG('C', 'F', 'F', ' ')));
        HbBlob cff2(hb_face_reference_table(face.get(), HB_TAG('C', 'F', 'F', '2')));
        const bool is_cff = hb_blob_get_length(cff.get()) > 0 || hb_blob_get_length(cff2.get()) > 0;
        all.push_back({it->path(), i, name_of(face.get(), HB_OT_NAME_ID_FONT_FAMILY),
                       name_of(face.get(), HB_OT_NAME_ID_FONT_SUBFAMILY), is_cff, hangul,
                       hb_face_get_glyph_count(face.get())});
      }
    }
  }
  const auto ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  int hangul = 0, hangul_glyf = 0;
  for (const auto& c : all) {
    if (!c.has_hangul) continue;
    ++hangul;
    if (!c.cff) ++hangul_glyf;
    std::cout << "[scan]   " << c.family << " / " << c.style << " [" << (c.cff ? "CFF" : "glyf")
              << "] " << c.path.filename() << "#" << c.index << " glyphs=" << c.glyphs << "\n";
  }
  std::cout << "[scan] files=" << files << " faces=" << all.size() << " hangul=" << hangul
            << " (glyf " << hangul_glyf << ", CFF " << hangul - hangul_glyf << ") in " << ms
            << " ms\n";
}

}  // namespace
}  // namespace onpdf
