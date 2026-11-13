cmake_minimum_required(VERSION 3.25)

project(OnPdf
  VERSION 0.0.1
  DESCRIPTION "PDFium 기반 PDF 뷰어·편집기"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 모든 실행 파일과 공유 라이브러리를 한 곳에 모은다 (PDFium DLL 복사 위치)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(ONPDF_BUILD_APP   "Qt UI(app) 빌드 — Qt 6 필요" OFF)
option(ONPDF_BUILD_CLI   "명령줄 도구(cli) 빌드" ON)
option(ONPDF_BUILD_TESTS "테스트 빌드" ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(OnPdfWarnings)
include(OnPdfPdfium)

add_subdirectory(core)
add_subdirectory(edit)

if(ONPDF_BUILD_CLI)
  add_subdirectory(cli)
endif()

if(ONPDF_BUILD_APP)
  add_subdirectory(app)
endif()

if(ONPDF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
