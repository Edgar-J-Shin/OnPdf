// Spike #59: (1) 실행 취소로 지운 텍스트의 폰트 사전을 저장 파일에서 떨어뜨릴 수 있는가 (2) OTF(CFF)·TTC 서브셋 임베딩.
// 제품 코드가 아니다 — qpdf 헤더를 engine 밖에서 직접 쓴다(스파이크 예외). 결론만 01-spike.md 로 옮긴다.
//
// 실행: ONPDF_SPIKE_FONT_DIR=<otf/ttc 가 있는 폴더> onpdf_spike_font_cleanup
#include "onpdf/core/engine/pdfium_document.h"
#include "onpdf/core/engine/qpdf_tools.h"
#include "onpdf/core/engine/save.h"
#include "onpdf/edit/add_text_command.h"
#include "onpdf/edit/font_face.h"

#include "../test_support.h"

#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFWriter.hh>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace onpdf {
namespace {

using engine::PdfiumDocument;
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

// qpdf 로 다시 쓴다: 기본 QPDFWriter 는 트레일러에서 닿는 객체만 쓴다(미참조 객체 폐기)
std::vector<std::uint8_t> qpdf_rewrite(const std::vector<std::uint8_t>& in, bool object_streams, bool preserve_encryption = true) {
  QPDF q;
  q.processMemoryFile("in.pdf", reinterpret_cast<const char*>(in.data()), in.size());
  QPDFWriter w(q);
  w.setOutputMemory();
  if (object_streams) w.setObjectStreamMode(qpdf_o_generate);
  else w.setObjectStreamMode(qpdf_o_preserve);
  w.setPreserveEncryption(preserve_encryption);
  w.write();
  auto buf = w.getBufferSharedPointer();
  return {buf->getBuffer(), buf->getBuffer() + buf->getSize()};
}

struct Census {
  int fonts = 0;        // /Type /Font 사전 수(전체)
  int font_files = 0;   // /FontFile2 스트림 수
  long objects = 0;
};
Census census(const std::vector<std::uint8_t>& in) {
  Census c;
  QPDF q;
  q.processMemoryFile("in.pdf", reinterpret_cast<const char*>(in.data()), in.size());
  for (auto& oh : q.getAllObjects()) {
    ++c.objects;
    if (oh.isDictionaryOfType("/Font")) ++c.fonts;
    if (oh.isDictionary() && oh.hasKey("/FontFile2")) ++c.font_files;
    if (oh.isStream() && oh.getDict().hasKey("/FontFile2")) ++c.font_files;
  }
  return c;
}

std::shared_ptr<const edit::FontFace> load_font(const std::filesystem::path& p) {
  auto f = edit::FontFace::load(p);
  if (!f) {
    std::cerr << "font load failed: " << p << ": " << f.error().message << "\n";
    return nullptr;
  }
  return *f;
}

edit::TextBoxSpec box(std::shared_ptr<const edit::FontFace> face, const std::string& text, double size = 24.0) {
  edit::TextBoxSpec s;
  s.page = 0;
  s.origin = {72, 100};
  s.size = size;
  s.color = {0, 0, 0, 255};
  s.utf8 = text;
  s.face = std::move(face);
  return s;
}

// 비트맵에서 흰색이 아닌 픽셀 수 (렌더 확인)
long dark_pixels(const Bitmap& b) {
  long n = 0;
  for (int y = 0; y < b.height; ++y)
    for (int x = 0; x < b.width; ++x) {
      const auto* p = b.row(y) + x * 4;
      if (p[0] < 128 && p[1] < 128 && p[2] < 128) ++n;
    }
  return n;
}

// ---------- (1) 폰트 정리 ----------

TEST(FontCleanupSpike, UndoneTextLeavesFontAndQpdfRewriteDropsIt) {
  auto face = load_font(testdata("fonts") / "NotoSansKR-subset.ttf");
  ASSERT_TRUE(face);
  const auto original = read_bytes(testdata("simple.pdf"));
  ScopedTempDir dir;

  auto doc = PdfiumDocument::open_from_memory(original);
  ASSERT_TRUE(doc);
  edit::AddTextCommand cmd(box(face, "한글 텍스트 추가"));
  ASSERT_TRUE(cmd.execute(*doc));
  ASSERT_TRUE(cmd.undo(*doc));
  EXPECT_EQ(doc->font_cache_size(), 1) << "엔진 캐시에는 남아 있다";
  const auto saved_path = dir / "undone.pdf";
  ASSERT_TRUE(doc->save_to(saved_path, false));
  const auto saved = read_bytes(saved_path);
  const Census before = census(saved);

  const auto t0 = std::chrono::steady_clock::now();
  const auto rewritten = qpdf_rewrite(saved, false);
  const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const Census after = census(rewritten);
  const auto rewritten_path = dir / "rewritten.pdf";
  write_bytes(rewritten_path, rewritten);
  auto check = engine::QpdfTools::check(rewritten_path);
  ASSERT_TRUE(check) << check.error().message;
  auto reopened = PdfiumDocument::open(rewritten_path);
  ASSERT_TRUE(reopened);

  std::cout << "[cleanup] original=" << original.size() << " saved_after_undo=" << saved.size()
            << " (fonts=" << before.fonts << ", font_files=" << before.font_files << ", objects=" << before.objects << ")"
            << " qpdf_rewrite=" << rewritten.size() << " (fonts=" << after.fonts << ", font_files=" << after.font_files
            << ", objects=" << after.objects << ") " << ms << "ms, qpdf_warnings=" << check->warnings.size() << "\n";
  EXPECT_GT(saved.size(), original.size() + 2000) << "취소해도 폰트가 남는다 (현재 제한 재현)";
  EXPECT_EQ(after.font_files, 0) << "qpdf 재작성이 미참조 FontFile2 를 떨어뜨린다";
  EXPECT_LT(rewritten.size(), original.size() + 2000);
}

TEST(FontCleanupSpike, QpdfRewriteKeepsUsedFontAndRender) {
  auto face = load_font(testdata("fonts") / "NotoSansKR-subset.ttf");
  ASSERT_TRUE(face);
  ScopedTempDir dir;
  auto doc = PdfiumDocument::open_from_memory(read_bytes(testdata("simple.pdf")));
  ASSERT_TRUE(doc);
  edit::AddTextCommand first(box(face, "첫 줄"));
  ASSERT_TRUE(first.execute(*doc));
  ASSERT_TRUE(first.undo(*doc));  // 다른 글자 집합 → 다른 서브셋 키 → 미참조 폰트 1개
  edit::AddTextCommand kept(box(face, "둘째 줄 텍스트"));
  ASSERT_TRUE(kept.execute(*doc));
  EXPECT_EQ(doc->font_cache_size(), 2);
  const auto saved_path = dir / "kept.pdf";
  ASSERT_TRUE(doc->save_to(saved_path, false));
  const auto saved = read_bytes(saved_path);
  engine::RenderOptions ro;
  ro.scale = 2.0f;
  auto base = doc->render_page(0, ro);
  ASSERT_TRUE(base);

  for (bool object_streams : {false, true}) {
    const auto rewritten = qpdf_rewrite(saved, object_streams);
    const auto p = dir / (object_streams ? "kept-os.pdf" : "kept-plain.pdf");
    write_bytes(p, rewritten);
    auto check = engine::QpdfTools::check(p);
    ASSERT_TRUE(check);
    auto re = PdfiumDocument::open(p);
    ASSERT_TRUE(re);
    auto bmp = re->render_page(0, ro);
    ASSERT_TRUE(bmp);
    ASSERT_EQ(bmp->pixels, base->pixels) << "재작성 뒤 렌더가 같다";
    auto text = re->load_text(0);
    ASSERT_TRUE(text);
    EXPECT_NE(text->all_text().find("둘째 줄 텍스트"), std::string::npos);
    auto objs = re->text_objects(0);
    ASSERT_TRUE(objs);
    EXPECT_EQ(objs->size(), 1u) << "마크 콘텐츠가 보존된다";
    const Census c = census(rewritten);
    std::cout << "[keep] object_streams=" << object_streams << " saved=" << saved.size() << " rewritten=" << rewritten.size()
              << " fonts=" << c.fonts << " font_files=" << c.font_files << "\n";
    EXPECT_EQ(c.font_files, 1);
  }
}

TEST(FontCleanupSpike, EncryptedDocumentSurvivesRewrite) {
  // 암호 문서: qpdf 는 암호를 알아야 다시 쓴다. 암호 유지(setPreserveEncryption) 로 결과도 암호 문서
  const auto in = read_bytes(testdata("encrypted-aes256.pdf"));
  QPDF q;
  q.processMemoryFile("in.pdf", reinterpret_cast<const char*>(in.data()), in.size(), "user");
  QPDFWriter w(q);
  w.setOutputMemory();
  w.setPreserveEncryption(true);
  w.write();
  auto buf = w.getBufferSharedPointer();
  ScopedTempDir dir;
  const auto p = dir / "enc.pdf";
  write_bytes(p, {buf->getBuffer(), buf->getBuffer() + buf->getSize()});
  EXPECT_FALSE(PdfiumDocument::open(p)) << "여전히 암호가 필요하다";
  auto with = PdfiumDocument::open(p, "user");
  ASSERT_TRUE(with) << with.error().message;
  auto check = engine::QpdfTools::check(p, "user");
  EXPECT_TRUE(check);
}

TEST(FontCleanupSpike, IncrementalSaveOfSignedDocumentIsNotRewritable) {
  // 서명 문서는 증분 저장: 폰트 사전이 남아도 재작성하면 서명이 깨진다 → 정리 대상에서 제외해야 한다 (측정만)
  auto face = load_font(testdata("fonts") / "NotoSansKR-subset.ttf");
  ASSERT_TRUE(face);
  const auto original = read_bytes(testdata("signed-dummy.pdf"));
  auto doc = PdfiumDocument::open_from_memory(original);
  ASSERT_TRUE(doc);
  ASSERT_EQ(doc->signature_count(), 1);
  edit::AddTextCommand cmd(box(face, "문서 서문"));
  ASSERT_TRUE(cmd.execute(*doc));
  ASSERT_TRUE(cmd.undo(*doc));
  ScopedTempDir dir;
  const auto p = dir / "signed-inc.pdf";
  ASSERT_TRUE(doc->save_to(p, true));
  const auto saved = read_bytes(p);
  ASSERT_TRUE(std::equal(original.begin(), original.end(), saved.begin()));
  std::cout << "[signed] original=" << original.size() << " incremental=" << saved.size() << " (+" << saved.size() - original.size()
            << " bytes: 폰트 포함)\n";
  // 증분 저장은 원본 뒤에 덧붙이므로 바뀐 객체만 쓴다 — 미참조 폰트는 새 객체라 함께 붙는다. 재작성하면 원본 바이트가 바뀌어 서명 무효
}

// ---------- (2) OTF(CFF)·TTC ----------

struct FontCase {
  const char* file;
  const char* text;
  const char* note;
};

void embed_and_verify(const std::filesystem::path& font_path, const std::string& text, const char* note) {
  auto face = load_font(font_path);
  if (!face) GTEST_SKIP() << "font not loadable: " << font_path;
  std::cout << "[font] " << note << " file=" << font_path.filename() << " family=" << face->family_name() << " upem=" << face->upem()
            << " missing=" << face->missing(text).size() << "\n";
  ScopedTempDir dir;
  auto doc = PdfiumDocument::open_from_memory(read_bytes(testdata("simple.pdf")));
  ASSERT_TRUE(doc);
  edit::AddTextCommand cmd(box(face, text));
  auto exec = cmd.execute(*doc);
  if (!exec) {
    std::cout << "[font]   execute failed: " << exec.error().message << "\n";
    FAIL() << exec.error().message;
  }
  const auto p = dir / "embedded.pdf";
  ASSERT_TRUE(doc->save_to(p, false));
  const auto saved = read_bytes(p);
  // FontFile2 안의 데이터 태그
  std::string tag = "?";
  {
    QPDF q;
    q.processMemoryFile("in.pdf", reinterpret_cast<const char*>(saved.data()), saved.size());
    for (auto& oh : q.getAllObjects()) {
      if (oh.isDictionary() && oh.hasKey("/FontFile2")) {
        auto data = oh.getKey("/FontFile2").getStreamData();
        tag.assign(reinterpret_cast<const char*>(data->getBuffer()), 4);
        if (tag[0] == 0) tag = "0100(glyf)";
        std::cout << "[font]   FontFile2 bytes=" << data->getSize() << " tag=" << tag << "\n";
      }
    }
  }
  auto check = engine::QpdfTools::check(p);
  EXPECT_TRUE(check) << check.error().message;
  auto re = PdfiumDocument::open(p);
  ASSERT_TRUE(re);
  engine::RenderOptions ro;
  ro.scale = 2.0f;
  auto bmp = re->render_page(0, ro);
  ASSERT_TRUE(bmp);
  auto plain = PdfiumDocument::open_from_memory(read_bytes(testdata("simple.pdf")));
  auto base = plain->render_page(0, ro);
  const long added = dark_pixels(*bmp) - dark_pixels(*base);
  auto extracted = re->load_text(0);
  ASSERT_TRUE(extracted);
  const bool text_found = extracted->all_text().find(text) != std::string::npos;
  std::cout << "[font]   saved=" << saved.size() << " dark_px_added=" << added << " extracted=" << (text_found ? "yes" : "NO")
            << " qpdf=" << (check ? "ok" : "fail") << "\n";
  std::filesystem::copy_file(p, std::filesystem::temp_directory_path() / (std::string("onpdf-spike59-") + note + ".pdf"),
                             std::filesystem::copy_options::overwrite_existing);
  EXPECT_GT(added, 200) << "글자가 그려져야 한다";
  EXPECT_TRUE(text_found);
}

TEST(FontCleanupSpike, OtfCffKorean) {
  if (font_dir().empty()) GTEST_SKIP();
  embed_and_verify(font_dir() / "NotoSerifKR-Regular.otf", "명조 OTF 한글 Serif 123", "otf-cff-serif");
}
TEST(FontCleanupSpike, OtfCffSansKorean) {
  if (font_dir().empty()) GTEST_SKIP();
  embed_and_verify(font_dir() / "NotoSansKR-Regular.otf", "고딕 OTF 한글 Sans 123", "otf-cff-sans");
}
TEST(FontCleanupSpike, TtcCffKorean) {
  if (font_dir().empty()) GTEST_SKIP();
  embed_and_verify(font_dir() / "NotoSansCJK-Regular.ttc", "TTC(CFF) 한글 첫 번째 면", "ttc-cff");
}
TEST(FontCleanupSpike, TtcGlyfLatin) {
  embed_and_verify("/System/Library/Fonts/Helvetica.ttc", "TTC glyf Helvetica 123", "ttc-glyf");
}
TEST(FontCleanupSpike, TtfGlyfLatinUnicode) {
  embed_and_verify("/System/Library/Fonts/Supplemental/Arial Unicode.ttf", "TTF glyf 한글 Arial Unicode", "ttf-glyf");
}

}  // namespace
}  // namespace onpdf
