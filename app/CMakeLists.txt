# Qt 6 Widgets UI. ONPDF_BUILD_APP=ON (preset dev-app) 일 때만 빌드한다.
# 사전 준비: export QT_ROOT=$(brew --prefix qt)   (preset 이 CMAKE_PREFIX_PATH 로 넘긴다)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets PrintSupport)
qt_standard_project_setup()
# .ui 파일을 쓰지 않는다. AUTOUIC 가 켜져 있으면 "ui_*.h" 헤더를 uic 산출물로 오해한다.
set(CMAKE_AUTOUIC OFF)

# 메인 창을 제외한 로직·위젯 (테스트 대상).
add_library(onpdf_app_lib STATIC
  ui_dispatcher.h ui_dispatcher.cpp
  qt_image.h qt_image.cpp
  document_session.h document_session.cpp
  document_tile_source.h document_tile_source.cpp
  document_view.h document_view.cpp
  search_bar.h search_bar.cpp
  outline_panel.h outline_panel.cpp
  thumbnail_panel.h thumbnail_panel.cpp
  print_job.h print_job.cpp
  edit_controller.h edit_controller.cpp
  annotation_dialog.h annotation_dialog.cpp
  image_import.h image_import.cpp
  form_field_editor.h form_field_editor.cpp
  structure_job.h structure_job.cpp
  main_window.h main_window.cpp
)
target_include_directories(onpdf_app_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(onpdf_app_lib PUBLIC onpdf::core onpdf::edit Qt6::Core Qt6::Gui Qt6::Widgets Qt6::PrintSupport)
onpdf_set_warnings(onpdf_app_lib)

qt_add_executable(onpdf_app
  main.cpp
)
set_target_properties(onpdf_app PROPERTIES MACOSX_BUNDLE OFF WIN32_EXECUTABLE ON)
target_link_libraries(onpdf_app PRIVATE onpdf_app_lib Qt6::Widgets)
target_compile_definitions(onpdf_app PRIVATE ONPDF_VERSION="${PROJECT_VERSION}")
onpdf_set_warnings(onpdf_app)
onpdf_set_runtime_rpath(onpdf_app)

# 번들 글꼴 (#45): third_party/fetch_fonts.py 가 받아 둔 Noto Sans KR 을 실행 파일 옆 fonts/ 로 복사한다. 없으면 경고만 (텍스트 도구 비활성)
set(ONPDF_BUNDLED_FONT "${CMAKE_SOURCE_DIR}/third_party/fonts/NotoSansKR[wght].ttf")
if(EXISTS "${ONPDF_BUNDLED_FONT}")
  add_custom_command(TARGET onpdf_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:onpdf_app>/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ONPDF_BUNDLED_FONT}" "${CMAKE_SOURCE_DIR}/third_party/fonts/OFL-NotoSansKR.txt" "$<TARGET_FILE_DIR:onpdf_app>/fonts/"
    COMMENT "Copying bundled font (Noto Sans KR, OFL)" VERBATIM)
else()
  message(WARNING "번들 글꼴이 없습니다: python3 third_party/fetch_fonts.py 를 먼저 실행하세요 (텍스트 추가 도구가 비활성됩니다)")
endif()
