#pragma once

// 시스템 글꼴 폴더 (#73 폰트 대체). 플랫폼 분기는 core/src/platform/font_dirs.cpp 에만 있다.
// 존재하지 않는 폴더도 돌려줄 수 있다 — 호출자(edit::FontCatalog::scan)가 건너뛴다.

#include <filesystem>
#include <vector>

namespace onpdf::platform {

struct FontDir {
  std::filesystem::path path;
  bool recursive = false;  // Linux 글꼴 폴더는 하위 폴더에 나뉘어 있다
};

[[nodiscard]] std::vector<FontDir> system_font_dirs();

}  // namespace onpdf::platform
