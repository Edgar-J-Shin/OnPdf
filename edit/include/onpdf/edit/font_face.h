#pragma once

// 글꼴 서비스 (HarfBuzz, PDFium 없음): TTF 로드, 글자 배치(GID·전진폭), 글리프 없음 탐지,
// 서브셋(GID 재배치), ToUnicode CMap 생성. 엔진에는 바이트·CID 만 넘긴다 (#43: PDFium 의
// 서브셋·자동 ToUnicode 를 쓰지 않는다). FontFace 는 불변이라 스레드 간 공유(shared_ptr<const>)해도
// 된다. 배치·서브셋의 가변 객체는 호출 안에서만 산다.

#include "onpdf/core/error.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace onpdf::edit {

struct ShapedGlyph {
  std::uint32_t gid = 0;   // 원본 폰트의 글리프 ID
  std::u32string text;     // 그 글리프를 만든 입력 글자들 (클러스터 전체: NFD 한글·결합 문자·합자도
                           // ToUnicode 에 그대로)
  double x_advance = 0.0;  // pt
};

struct ShapedText {
  std::vector<ShapedGlyph> glyphs;
  double width = 0.0;    // Σ x_advance (pt)
  double ascent = 0.0;   // pt (양수)
  double descent = 0.0;  // pt (양수)
};

// 서브셋 결과. GID 가 촘촘히 재배치되므로 CID = 새 GID 로 쓴다
struct FontSubset {
  std::vector<std::uint8_t> ttf;
  std::unordered_map<std::uint32_t, std::uint32_t> old_to_new;
  std::vector<std::uint8_t> cid_to_gid;  // Identity (2바이트 big-endian × 글리프 수)
  std::string key;                       // ttf 의 FNV-1a 64 (16진) — 문서 폰트 캐시 키
  unsigned glyph_count = 0;
};

class FontFace {
 public:
  ~FontFace();
  FontFace(const FontFace&) = delete;
  FontFace& operator=(const FontFace&) = delete;

  // TTF/OTF(CFF)/TTC 파일. face_index 는 TTC 의 면 (단일 글꼴은 0). 실패:
  // kFileNotFound·kInvalidFormat(면 번호 범위 밖 포함)
  [[nodiscard]] static Result<std::shared_ptr<const FontFace>> load(
      const std::filesystem::path& path, unsigned face_index = 0);
  [[nodiscard]] static Result<std::shared_ptr<const FontFace>> load_from_memory(
      std::vector<std::uint8_t> bytes, unsigned face_index = 0);

  // 한 줄(개행 없음)을 size_pt 로 배치. 변형 폰트는 wght=400 으로 고정.
  // 탭·NBSP 처럼 글리프가 없는 레이아웃 공백은 공백 글리프로 바꾼다 (탭 = 공백 4칸)
  [[nodiscard]] ShapedText shape(std::string_view utf8, double size_pt) const;
  // 글리프가 없는 글자 (공백·탭·개행 제외, 중복 제거, 입력 순서)
  [[nodiscard]] std::u32string missing(std::string_view utf8) const;
  // gids(원본 GID) 만 남긴 정적 서브셋. .notdef(0) 는 항상 포함
  [[nodiscard]] Result<FontSubset> subset(const std::vector<std::uint32_t>& gids) const;
  [[nodiscard]] unsigned upem() const noexcept;
  [[nodiscard]] bool has_weight_axis() const noexcept;
  [[nodiscard]] std::string family_name() const;
  [[nodiscard]] std::string style_name() const;  // name ID 2 ("Regular", "Bold"…)
  [[nodiscard]] bool is_cff()
      const noexcept;  // CFF/CFF2 외곽선 — 서브셋이 OTTO 로 나온다 (#73 저장 후처리 대상)
  [[nodiscard]] unsigned face_index() const noexcept;

 private:
  struct Impl;
  explicit FontFace(std::unique_ptr<Impl> impl);
  // blob(파일 mmap 또는 메모리)이 든 Impl 에서 face 를 연다
  [[nodiscard]] static Result<std::shared_ptr<const FontFace>> open_face(std::unique_ptr<Impl> impl,
                                                                         unsigned face_index);
  std::unique_ptr<Impl> impl_;
};

// ToUnicode CMap (bfchar, CID 오름차순, 같은 CID 는 먼저 나온 문자열). dst 는 UTF-16BE (여러
// 코드·서로게이트 허용)
[[nodiscard]] std::string make_to_unicode_cmap(
    std::vector<std::pair<std::uint32_t, std::u32string>> cid_to_unicode);
// 레이아웃 공백 (공백·탭·NBSP·CR·LF)
[[nodiscard]] bool is_layout_whitespace(char32_t c) noexcept;

// UTF-8 → 코드 포인트 (잘못된 바이트는 U+FFFD)
[[nodiscard]] std::u32string decode_utf8(std::string_view utf8);

}  // namespace onpdf::edit
