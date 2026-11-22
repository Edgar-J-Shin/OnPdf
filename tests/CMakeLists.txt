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
  unit/geometry_test.cpp
  unit/tile_test.cpp
  unit/page_layout_test.cpp
  unit/tile_cache_test.cpp
  unit/tile_scheduler_test.cpp
  unit/text_index_test.cpp
  unit/text_selection_test.cpp
  unit/text_page_test.cpp
  unit/search_test.cpp
  unit/outline_test.cpp
  unit/print_layout_test.cpp
  unit/undo_stack_test.cpp
  unit/rotate_page_test.cpp
  unit/page_ops_test.cpp
  unit/delete_pages_test.cpp
  unit/move_pages_test.cpp
  unit/insert_pages_test.cpp
  unit/annotation_test.cpp
  unit/annotation_commands_test.cpp
  unit/font_face_test.cpp
  unit/font_catalog_test.cpp
  unit/cff_charset_test.cpp
  unit/text_layout_test.cpp
  unit/add_text_command_test.cpp
  unit/text_object_test.cpp
  unit/text_box_commands_test.cpp
  unit/text_line_test.cpp
  unit/text_line_commands_test.cpp
  unit/page_object_test.cpp
  unit/form_test.cpp
  unit/object_commands_test.cpp
  unit/form_commands_test.cpp
  unit/page_ranges_test.cpp
  unit/qpdf_tools_test.cpp
  unit/save_test.cpp
  unit/fs_sync_test.cpp
)
target_compile_definitions(onpdf_unit_tests PRIVATE ONPDF_TEST_GLOBAL_PDFIUM=1)
target_link_libraries(onpdf_unit_tests PRIVATE onpdf::edit)
target_include_directories(onpdf_unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/core/src/engine)  # cff_charset·fnv 내부 헤더 (#73)

onpdf_add_test(onpdf_render_tests
  render/render_regression_test.cpp
  render/tile_stitch_test.cpp
)
target_compile_definitions(onpdf_render_tests PRIVATE ONPDF_TEST_GLOBAL_PDFIUM=1)
target_link_libraries(onpdf_render_tests PRIVATE onpdf::edit)

# PdfiumWorker 가 자기 스레드에서 라이브러리를 만들므로 전역 환경 없이 분리한다
# 왕복: 열기 → 저장 → 다시 열기 (manifest 전체)
onpdf_add_test(onpdf_roundtrip_tests
  roundtrip/roundtrip_test.cpp
)
target_compile_definitions(onpdf_roundtrip_tests PRIVATE ONPDF_TEST_GLOBAL_PDFIUM=1)
target_link_libraries(onpdf_roundtrip_tests PRIVATE onpdf::edit)

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
    app/document_view_test.cpp
    app/search_test.cpp
    app/sidebar_test.cpp
    app/print_test.cpp
    app/edit_test.cpp
    app/annotation_test.cpp
    app/merge_split_test.cpp
    app/text_tool_test.cpp
    app/text_line_tool_test.cpp
    app/image_tool_test.cpp
    app/form_tool_test.cpp
  )
  target_link_libraries(onpdf_app_tests PRIVATE onpdf_app_lib)
  set_tests_properties(onpdf_app_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

# ---- Spike (#43): 한글 폰트 임베딩. 제품 코드가 아니며 PDFium 헤더를 직접 쓴다. ctest 에는 넣지 않는다 ----
if(ONPDF_BUILD_SPIKES)
  add_executable(onpdf_spike_korean_text spike/korean_text_spike.cpp spike/image_object_spike.cpp spike/form_fill_spike.cpp)
  target_link_libraries(onpdf_spike_korean_text PRIVATE onpdf::core GTest::gtest GTest::gtest_main pdfium)
  target_compile_definitions(onpdf_spike_korean_text PRIVATE ONPDF_TESTDATA_DIR="${ONPDF_TESTDATA_DIR}" ONPDF_TEST_GLOBAL_PDFIUM=1)
  onpdf_set_warnings(onpdf_spike_korean_text)
  onpdf_set_runtime_rpath(onpdf_spike_korean_text)

  # Spike (#55): 기존 텍스트 줄 단위 수정 — 코퍼스 측정 도구(gtest 아님)
  add_executable(onpdf_spike_text_edit spike/text_edit_spike.cpp spike/text_edit_surgery.cpp)
  # qpdf 는 onpdf_core 가 이미 링크한다(정적). 헤더 경로만 가져온다 — 링크 인터페이스의 맨이름 z;jpeg 를 피하려고
  find_package(qpdf CONFIG REQUIRED)
  if(TARGET qpdf::libqpdf)
    set(_spike_qpdf qpdf::libqpdf)
  else()
    set(_spike_qpdf qpdf::libqpdf_static)
  endif()
  get_target_property(_spike_qpdf_inc ${_spike_qpdf} INTERFACE_INCLUDE_DIRECTORIES)
  target_include_directories(onpdf_spike_text_edit PRIVATE ${_spike_qpdf_inc})
  target_link_libraries(onpdf_spike_text_edit PRIVATE onpdf::core pdfium)
  onpdf_set_warnings(onpdf_spike_text_edit)
  onpdf_set_runtime_rpath(onpdf_spike_text_edit)

  # Spike (#55 후속): M3 객체 편집의 재생성 손실 — 제품 엔진으로 측정
  add_executable(onpdf_spike_m3_regen spike/m3_regen_spike.cpp)
  target_link_libraries(onpdf_spike_m3_regen PRIVATE onpdf::core)
  onpdf_set_warnings(onpdf_spike_m3_regen)
  onpdf_set_runtime_rpath(onpdf_spike_m3_regen)

  # Spike (#59): 미참조 폰트 정리(qpdf 재작성)·OTF/TTC 서브셋 임베딩 — core+edit, qpdf 헤더 직접
  add_executable(onpdf_spike_font_cleanup spike/font_cleanup_spike.cpp)
  target_include_directories(onpdf_spike_font_cleanup PRIVATE ${_spike_qpdf_inc})
  target_link_libraries(onpdf_spike_font_cleanup PRIVATE onpdf::core onpdf::edit GTest::gtest GTest::gtest_main)
  target_compile_definitions(onpdf_spike_font_cleanup PRIVATE ONPDF_TESTDATA_DIR="${ONPDF_TESTDATA_DIR}" ONPDF_TEST_GLOBAL_PDFIUM=1)
  onpdf_set_warnings(onpdf_spike_font_cleanup)
  onpdf_set_runtime_rpath(onpdf_spike_font_cleanup)
endif()
