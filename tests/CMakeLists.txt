find_package(GTest CONFIG REQUIRED)

set(ONPDF_TESTDATA_DIR "${CMAKE_SOURCE_DIR}/testdata")

# onpdf_add_test(<name> [NO_GTEST_MAIN] <sources...>)
function(onpdf_add_test name)
  cmake_parse_arguments(ARG "NO_GTEST_MAIN" "" "" ${ARGN})
  add_executable(${name} ${ARG_UNPARSED_ARGUMENTS})
  target_link_libraries(${name} PRIVATE onpdf::core GTest::gtest)
  if(NOT ARG_NO_GTEST_MAIN)
    target_link_libraries(${name} PRIVATE GTest::gtest_main)
  endif()
  target_compile_definitions(${name} PRIVATE ONPDF_TESTDATA_DIR="${ONPDF_TESTDATA_DIR}")
  onpdf_set_warnings(${name})
  onpdf_set_runtime_rpath(${name})
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

# 메인 스레드에 전역 PdfiumLibrary 를 두고 PdfiumDocument 를 직접 쓰는 테스트
onpdf_add_test(onpdf_unit_tests
  unit/pdfium_document_test.cpp
  unit/png_io_test.cpp
  unit/testdata_manifest_test.cpp
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

# Qt app 로직 테스트 (위젯 없음, offscreen 플랫폼). ONPDF_BUILD_APP=ON 일 때만.
if(ONPDF_BUILD_APP)
  onpdf_add_test(onpdf_app_tests NO_GTEST_MAIN
    app/test_main.cpp
    app/qt_image_test.cpp
    app/document_session_test.cpp
  )
  target_link_libraries(onpdf_app_tests PRIVATE onpdf_app_lib)
  set_tests_properties(onpdf_app_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
