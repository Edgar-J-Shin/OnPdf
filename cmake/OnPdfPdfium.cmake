# third_party/fetch_pdfium.py 가 받아 둔 PDFium 사전 빌드를 imported target `pdfium` 으로 노출한다.
# 공유 라이브러리는 bin/ 으로 복사해 실행 파일 옆에서 찾도록 한다.

if(WIN32)
  set(_onpdf_os "win")
elseif(APPLE)
  set(_onpdf_os "mac")
else()
  set(_onpdf_os "linux")
endif()

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _onpdf_cpu)
if(_onpdf_cpu MATCHES "^(arm64|aarch64)$")
  set(_onpdf_arch "arm64")
else()
  set(_onpdf_arch "x64")
endif()

set(ONPDF_PDFIUM_PLATFORM "${_onpdf_os}-${_onpdf_arch}" CACHE STRING "pdfium-binaries 플랫폼 이름")
set(ONPDF_PDFIUM_DIR "${CMAKE_SOURCE_DIR}/third_party/pdfium/${ONPDF_PDFIUM_PLATFORM}")

if(NOT EXISTS "${ONPDF_PDFIUM_DIR}/PDFiumConfig.cmake")
  message(FATAL_ERROR
    "PDFium 바이너리가 없습니다: ${ONPDF_PDFIUM_DIR}\n"
    "먼저 실행: python3 third_party/fetch_pdfium.py --platform ${ONPDF_PDFIUM_PLATFORM}")
endif()

set(PDFium_DIR "${ONPDF_PDFIUM_DIR}")
find_package(PDFium REQUIRED CONFIG)

get_target_property(ONPDF_PDFIUM_SHARED_LIB pdfium IMPORTED_LOCATION)
file(COPY "${ONPDF_PDFIUM_SHARED_LIB}" DESTINATION "${CMAKE_BINARY_DIR}/bin")
message(STATUS "PDFium ${PDFium_VERSION} (${ONPDF_PDFIUM_PLATFORM}): ${ONPDF_PDFIUM_SHARED_LIB}")

# 실행 파일이 같은 디렉터리의 libpdfium 을 찾도록 rpath 설정
function(onpdf_set_runtime_rpath target)
  if(APPLE)
    set_target_properties(${target} PROPERTIES BUILD_RPATH "@loader_path")
  elseif(UNIX)
    set_target_properties(${target} PROPERTIES BUILD_RPATH "$ORIGIN")
  endif()
endfunction()
