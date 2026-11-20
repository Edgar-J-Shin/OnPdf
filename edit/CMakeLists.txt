# 편집 계층: Command · 실행 취소 스택 · 글꼴 서비스(HarfBuzz). core 만 의존한다 (Qt·PDFium 헤더 금지 — EditableDocument 인터페이스로 문서를 바꾼다)
# HarfBuzz 는 edit 에만 링크한다 (배치·측정·서브셋). FreeType 은 쓰지 않는다 — PDFium 공유 라이브러리가 FT_* 를 내보내 Linux 에서 충돌 위험 (#45)
find_package(harfbuzz CONFIG REQUIRED)
add_library(onpdf_edit STATIC
  src/undo_stack.cpp
  src/page_ops.cpp
  src/rotate_page.cpp
  src/delete_pages.cpp
  src/move_pages.cpp
  src/insert_pages.cpp
  src/annotation_commands.cpp
  src/font_face.cpp
  src/text_layout.cpp
  src/add_text_command.cpp
  src/text_box_commands.cpp
  src/object_commands.cpp
)
add_library(onpdf::edit ALIAS onpdf_edit)
target_include_directories(onpdf_edit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(onpdf_edit PUBLIC onpdf::core)
target_link_libraries(onpdf_edit PRIVATE harfbuzz::harfbuzz-subset harfbuzz::harfbuzz)
onpdf_set_warnings(onpdf_edit)
