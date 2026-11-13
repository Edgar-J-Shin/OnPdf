# Qt UI. ONPDF_BUILD_APP=ON (preset dev-app) 일 때만 빌드한다. M0 후반에 채운다.
find_package(Qt6 REQUIRED COMPONENTS Widgets)
message(FATAL_ERROR "app 타깃은 아직 구현되지 않았습니다 (M0 9단계)")
