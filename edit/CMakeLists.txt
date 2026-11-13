# M2 에서 Command / 실행 취소 스택이 들어온다. 지금은 인터페이스 타깃만 둔다.
add_library(onpdf_edit INTERFACE)
add_library(onpdf::edit ALIAS onpdf_edit)
target_link_libraries(onpdf_edit INTERFACE onpdf::core)
