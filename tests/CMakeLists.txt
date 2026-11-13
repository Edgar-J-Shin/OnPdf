find_package(GTest CONFIG REQUIRED)

set(ONPDF_TESTDATA_DIR "${CMAKE_SOURCE_DIR}/testdata")

function(onpdf_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE onpdf::core GTest::gtest GTest::gtest_main)
  target_compile_definitions(${name} PRIVATE ONPDF_TESTDATA_DIR="${ONPDF_TESTDATA_DIR}")
  onpdf_set_warnings(${name})
  onpdf_set_runtime_rpath(${name})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

onpdf_add_test(onpdf_unit_tests
  unit/pdfium_document_test.cpp
  unit/png_io_test.cpp
)

onpdf_add_test(onpdf_render_tests
  render/render_regression_test.cpp
)
