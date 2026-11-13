add_library(onpdf_core STATIC
  src/engine/pdfium_library.cpp
  src/engine/pdfium_document.cpp
  src/engine/pdfium_worker.cpp
  src/engine/document_handle.cpp
  src/image/png_io.cpp
)
add_library(onpdf::core ALIAS onpdf_core)

target_include_directories(onpdf_core PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(onpdf_core PUBLIC pdfium Threads::Threads)

find_package(Stb REQUIRED)
target_include_directories(onpdf_core SYSTEM PRIVATE ${Stb_INCLUDE_DIR})

onpdf_set_warnings(onpdf_core)
