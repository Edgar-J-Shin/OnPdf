# Qt 6 Widgets UI. ONPDF_BUILD_APP=ON (preset dev-app) 일 때만 빌드한다.
# 사전 준비: export QT_ROOT=$(brew --prefix qt)   (preset 이 CMAKE_PREFIX_PATH 로 넘긴다)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets)
qt_standard_project_setup()
# .ui 파일을 쓰지 않는다. AUTOUIC 가 켜져 있으면 "ui_*.h" 헤더를 uic 산출물로 오해한다.
set(CMAKE_AUTOUIC OFF)

# 위젯이 없는 로직 (테스트 대상). Qt Core/Gui 만 의존.
add_library(onpdf_app_lib STATIC
  ui_dispatcher.h ui_dispatcher.cpp
  qt_image.h qt_image.cpp
  document_session.h document_session.cpp
)
target_include_directories(onpdf_app_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(onpdf_app_lib PUBLIC onpdf::core Qt6::Core Qt6::Gui)
onpdf_set_warnings(onpdf_app_lib)

qt_add_executable(onpdf_app
  main.cpp
  main_window.h main_window.cpp
  page_view.h page_view.cpp
)
set_target_properties(onpdf_app PROPERTIES MACOSX_BUNDLE OFF WIN32_EXECUTABLE ON)
target_link_libraries(onpdf_app PRIVATE onpdf_app_lib Qt6::Widgets)
target_compile_definitions(onpdf_app PRIVATE ONPDF_VERSION="${PROJECT_VERSION}")
onpdf_set_warnings(onpdf_app)
onpdf_set_runtime_rpath(onpdf_app)
