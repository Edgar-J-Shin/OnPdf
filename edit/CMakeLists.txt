# 편집 계층: Command · 실행 취소 스택. core 만 의존한다 (Qt·PDFium 헤더 금지 — EditableDocument 인터페이스로 문서를 바꾼다)
add_library(onpdf_edit STATIC
  src/undo_stack.cpp
  src/rotate_page.cpp
)
add_library(onpdf::edit ALIAS onpdf_edit)
target_include_directories(onpdf_edit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(onpdf_edit PUBLIC onpdf::core)
onpdf_set_warnings(onpdf_edit)
