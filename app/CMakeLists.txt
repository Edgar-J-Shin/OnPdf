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
  font_catalog_loader.h font_catalog_loader.cpp
  main_window.h main_window.cpp
  file_open_filter.h
)
target_include_directories(onpdf_app_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(onpdf_app_lib PUBLIC onpdf::core onpdf::edit Qt6::Core Qt6::Gui Qt6::Widgets Qt6::PrintSupport)
onpdf_set_warnings(onpdf_app_lib)

qt_add_executable(onpdf_app
  main.cpp
)
# macOS 는 앱 번들(onpdf_app.app — 배포 워크플로우가 OnPdf.app 으로 이름을 바꾼다, #90), Windows 는 콘솔 없는 GUI 실행 파일
set(ONPDF_MACOS_MIN "13.0" CACHE STRING "Info.plist LSMinimumSystemVersion (배포 워크플로우가 Qt 의 실제 최소 버전으로 넘긴다)")
set_target_properties(onpdf_app PROPERTIES
  WIN32_EXECUTABLE ON
  MACOSX_BUNDLE ON
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
  MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.edgar-j-shin.onpdf"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
  MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
  MACOSX_BUNDLE_COPYRIGHT "OnPdf contributors. Apache License 2.0")
target_link_libraries(onpdf_app PRIVATE onpdf_app_lib Qt6::Widgets)
target_compile_definitions(onpdf_app PRIVATE ONPDF_VERSION="${PROJECT_VERSION}")
onpdf_set_warnings(onpdf_app)
onpdf_set_runtime_rpath(onpdf_app)

# 글꼴·라이선스 자리: 번들은 Contents/Resources (글꼴 탐색이 applicationDirPath()/../Resources/fonts 를 본다), 그 밖에는 실행 파일 옆
if(APPLE)
  set(_onpdf_resource_dir "$<TARGET_BUNDLE_CONTENT_DIR:onpdf_app>/Resources")
  set(_onpdf_install_resources "onpdf_app.app/Contents/Resources")
  set(_onpdf_install_prefix "onpdf_app.app/Contents/Resources/")
else()
  set(_onpdf_resource_dir "$<TARGET_FILE_DIR:onpdf_app>")
  set(_onpdf_install_resources ".")
  set(_onpdf_install_prefix "")  # "./fonts" 는 CMP0177 경로 정규화 경고를 낸다
endif()

# 번들 글꼴 (#45): third_party/fetch_fonts.py 가 받아 둔 Noto Sans KR 을 fonts/ 로 복사한다. 없으면 경고만 (텍스트 도구 비활성)
set(ONPDF_BUNDLED_FONT "${CMAKE_SOURCE_DIR}/third_party/fonts/NotoSansKR[wght].ttf")
set(ONPDF_BUNDLED_FONT_LICENSE "${CMAKE_SOURCE_DIR}/third_party/fonts/OFL-NotoSansKR.txt")
if(EXISTS "${ONPDF_BUNDLED_FONT}")
  add_custom_command(TARGET onpdf_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_onpdf_resource_dir}/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ONPDF_BUNDLED_FONT}" "${ONPDF_BUNDLED_FONT_LICENSE}" "${_onpdf_resource_dir}/fonts/"
    COMMENT "Copying bundled font (Noto Sans KR, OFL)" VERBATIM)
else()
  message(WARNING "번들 글꼴이 없습니다: python3 third_party/fetch_fonts.py 를 먼저 실행하세요 (텍스트 추가 도구가 비활성됩니다)")
endif()

# ---- 설치 규칙 (#90): cmake --install 이 배포 폴더를 만든다. Qt 는 windeployqt/macdeployqt 가 채운다 ----
if(APPLE)
  install(TARGETS onpdf_app BUNDLE DESTINATION .)
  install(FILES "${ONPDF_PDFIUM_SHARED_LIB}" DESTINATION "onpdf_app.app/Contents/Frameworks")
else()
  if(WIN32)
    # PDFium·vcpkg(qpdf·zlib·jpeg·harfbuzz…) 공유 라이브러리를 실행 파일의 PE 가져오기 표에서 전이적으로 찾아 옆에 둔다.
    # $<TARGET_RUNTIME_DLLS> 는 직접 링크한 imported 타깃만 담아 harfbuzz(edit)·zlib·jpeg(qpdf) 가 빠졌다 (#90 리허설).
    # Qt 는 windeployqt 가, MSVC 런타임은 워크플로우가 넣는다. 시스템 DLL·API 집합은 제외
    get_filename_component(_onpdf_pdfium_bin "${ONPDF_PDFIUM_SHARED_LIB}" DIRECTORY)
    get_target_property(_onpdf_qtcore Qt6::Core IMPORTED_LOCATION_RELEASE)
    get_filename_component(_onpdf_qt_bin "${_onpdf_qtcore}" DIRECTORY)
    # 의존성 집합은 따로 설치한다 (거르기 옵션이 대상 종류·DESTINATION 보다 앞에 와야 한다 — CMake 문법)
    install(TARGETS onpdf_app RUNTIME_DEPENDENCY_SET onpdf_runtime_deps RUNTIME DESTINATION .)
    install(RUNTIME_DEPENDENCY_SET onpdf_runtime_deps
      DIRECTORIES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin" "${_onpdf_pdfium_bin}" "${_onpdf_qt_bin}"
      PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
      POST_EXCLUDE_REGEXES ".*[Ss]ystem32/.*" "/Qt6[A-Za-z]*\\.dll$" "/(msvcp|vcruntime|concrt)[0-9_a-z]*\\.dll$"
      RUNTIME DESTINATION .)
  else()
    install(TARGETS onpdf_app RUNTIME DESTINATION .)
    install(FILES "${ONPDF_PDFIUM_SHARED_LIB}" DESTINATION .)
  endif()
endif()
if(EXISTS "${ONPDF_BUNDLED_FONT}")
  install(FILES "${ONPDF_BUNDLED_FONT}" "${ONPDF_BUNDLED_FONT_LICENSE}" DESTINATION "${_onpdf_install_prefix}fonts")
endif()
install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
        DESTINATION "${_onpdf_install_resources}")
install(DIRECTORY "${ONPDF_PDFIUM_LICENSES_DIR}/" DESTINATION "${_onpdf_install_prefix}licenses/pdfium")
