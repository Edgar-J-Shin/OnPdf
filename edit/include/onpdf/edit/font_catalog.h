#pragma once

// 시스템 글꼴 카탈로그 (#73 폰트 대체, HarfBuzz 만 — PDFium·Qt 없음).
// 글꼴 폴더를 한 번 훑어 face 마다 이름·스타일·외곽선 형식·한글 여부를 모은 불변 스냅샷. 스캔은
// 어느 스레드에서든 호출해도 되고 (앱은 시작 뒤 별도 스레드에서 한 번), 결과는 shared_ptr<const> 로
// 공유한다. 글꼴 바이트는 들고 있지 않다 — 후보가 정해지면 FontFace::load(path, face_index) 로
// 연다.

#include "onpdf/core/platform/font_dirs.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace onpdf::edit {

struct FontEntry {
  std::filesystem::path path;
  unsigned face_index = 0;  // TTC 면
  std::string family;       // name ID 1
  std::string style;        // name ID 2
  bool cff = false;         // CFF/CFF2 외곽선 (저장 후처리가 필요 — 증분 저장에서는 후보 제외)
  bool hangul = false;      // '한'·'글' 둘 다 cmap 에 있다
  unsigned glyph_count = 0;
  int weight = 400;  // OS/2 usWeightClass (스타일 축이 있으면 기본값)
};

class FontCatalog {
 public:
  static constexpr unsigned kMinGlyphs =
      1000;  // LastResort(7 글리프로 모든 코드 대응)·기호 글꼴 제외
  // 비재귀(FontDir::recursive 면 재귀), .ttf/.otf/.ttc. 숨김 글꼴('.' 로 시작하는 family)·글리프 <
  // min_glyphs 는 제외. 읽을 수 없는 폴더·파일은 건너뛴다. 결과는 경로·면 순으로 정렬(결정적)
  // cancelled 가 true 를 돌려주면 남은 파일을 건너뛴다 (앱 종료 때 스캔 스레드를 기다리지 않게)
  [[nodiscard]] static FontCatalog scan(const std::vector<platform::FontDir>& dirs,
                                        unsigned min_glyphs = kMinGlyphs,
                                        const std::function<bool()>& cancelled = {});
  [[nodiscard]] static FontCatalog from_entries(std::vector<FontEntry> entries);  // 테스트·주입

  [[nodiscard]] const std::vector<FontEntry>& entries() const noexcept { return entries_; }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

  // base_font_name(서브셋 태그가 제거된 BaseFont, 예 "Helvetica", "Arial,Bold", "ArialMT")과 정규화
  // 이름이 같은 항목. Regular/400 우선, 그다음 glyf 우선, 그다음 면 번호. allow_cff 가 false 면 CFF
  // 제외
  [[nodiscard]] std::vector<const FontEntry*> match(std::string_view base_font_name,
                                                    bool allow_cff) const;
  // 한글 기본 Sans: 플랫폼 선호 목록(Apple SD Gothic Neo, 맑은 고딕, Noto Sans CJK KR, Noto Sans
  // KR, NanumGothic, AppleGothic, Gulim) 순. 없으면 한글이 있는 아무 glyf 글꼴, 그다음 아무 한글
  // 글꼴. 없으면 nullptr
  [[nodiscard]] const FontEntry* default_hangul_sans(bool allow_cff) const;

  // "ABCDEF+" 서브셋 태그, ',' 뒤 스타일, "-Regular"/"MT"/"PSMT"/"PS" 접미, 공백·하이픈·'_' 제거,
  // 소문자
  [[nodiscard]] static std::string normalize_name(std::string_view name);

 private:
  std::vector<FontEntry> entries_;
};

}  // namespace onpdf::edit
