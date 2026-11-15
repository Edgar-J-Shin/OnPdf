#pragma once

// 저장 안전성을 위한 파일 시스템 동작. 플랫폼별 구현은 core/src/platform/fs_sync.cpp 에만 있다.
//  - flush_file_to_disk: 파일 내용을 디스크까지 내린다 (전원이 꺼져도 남도록). rename 으로 교체하기 전에 부른다
//  - replace_file: 같은 볼륨 안에서 원자적으로 교체한다 (POSIX rename / Windows MoveFileExW + WRITE_THROUGH)
//  - flush_directory: 교체(이름 바꾸기) 자체를 디스크까지 내린다 (POSIX). Windows 는 할 일 없음

#include "onpdf/core/error.h"

#include <filesystem>

namespace onpdf::platform {

[[nodiscard]] Result<void> flush_file_to_disk(const std::filesystem::path& file);
[[nodiscard]] Result<void> replace_file(const std::filesystem::path& from, const std::filesystem::path& to);
[[nodiscard]] Result<void> flush_directory(const std::filesystem::path& dir);

}  // namespace onpdf::platform
