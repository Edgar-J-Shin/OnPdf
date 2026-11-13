add_executable(onpdf_cli main.cpp)
target_link_libraries(onpdf_cli PRIVATE onpdf::core)
onpdf_set_warnings(onpdf_cli)
onpdf_set_runtime_rpath(onpdf_cli)
