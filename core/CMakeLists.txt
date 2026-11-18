add_library(onpdf_core STATIC
  src/engine/pdfium_library.cpp
  src/engine/pdfium_document.cpp
  src/engine/pdfium_worker.cpp
  src/engine/document_handle.cpp
  src/engine/text_page.cpp
  src/engine/dest.cpp
  src/engine/save.cpp
  src/engine/qpdf_tools.cpp
  src/page_ranges.cpp
  src/image/png_io.cpp
  src/platform/fs_sync.cpp
  src/view/page_layout.cpp
  src/view/tile_cache.cpp
  src/view/tile_source.cpp
  src/view/tile_scheduler.cpp
  src/view/text_index.cpp
  src/view/text_selection.cpp
  src/view/search.cpp
  src/view/outline.cpp
  src/view/print_layout.cpp
)
add_library(onpdf::core ALIAS onpdf_core)

target_include_directories(onpdf_core PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(onpdf_core PUBLIC pdfium Threads::Threads)

find_package(Stb REQUIRED)
target_include_directories(onpdf_core SYSTEM PRIVATE ${Stb_INCLUDE_DIR})

# qpdf (병합·분할·검사). 헤더는 core/engine 안에서만 포함하므로 PRIVATE. 정적 트리플릿은 libqpdf_static 만 내보낼 수 있다
find_package(qpdf CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(JPEG REQUIRED)
if(TARGET qpdf::libqpdf)
  set(ONPDF_QPDF_TARGET qpdf::libqpdf)
else()
  set(ONPDF_QPDF_TARGET qpdf::libqpdf_static)
endif()
# vcpkg 의 qpdf 설정은 zlib·libjpeg-turbo 를 맨이름("z;jpeg")으로 적어 링커 검색 경로에 없으면 실패한다 → 실제 타깃으로 바꾼다
set_property(TARGET ${ONPDF_QPDF_TARGET} PROPERTY INTERFACE_LINK_LIBRARIES ZLIB::ZLIB JPEG::JPEG)
target_link_libraries(onpdf_core PRIVATE ${ONPDF_QPDF_TARGET})

onpdf_set_warnings(onpdf_core)
