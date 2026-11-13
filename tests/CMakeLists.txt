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

# 메인 스레드에 전역 PdfiumLibrary 를 두고 PdfiumDocument 를 직접 쓰는 테스트
onpdf_add_test(onpdf_unit_tests
  unit/pdfium_document_test.cpp
  unit/png_io_test.cpp
)
target_compile_definitions(onpdf_unit_tests PRIVATE ONPDF_TEST_GLOBAL_PDFIUM=1)

onpdf_add_test(onpdf_render_tests
  render/render_regression_test.cpp
)
target_compile_definitions(onpdf_render_tests PRIVATE ONPDF_TEST_GLOBAL_PDFIUM=1)

# PdfiumWorker 가 자기 스레드에서 라이브러리를 만들므로 전역 환경 없이 분리한다
onpdf_add_test(onpdf_worker_tests
  unit/pdfium_worker_test.cpp
  unit/document_handle_test.cpp
)
