# Intel macOS 용 vcpkg 트리플릿 (#108): x64-osx 와 같되 배포 타깃을 macOS 13 으로 고정한다.
# vcpkg 가 빌드하는 qpdf·harfbuzz·zlib·libjpeg 의 minos 가 앱 최소 버전(ONPDF_MACOS_MIN)을 넘지 않게.
# arm64 러너에서 교차 빌드한다 (CMAKE_OSX_ARCHITECTURES=x86_64).
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES x86_64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 13.0)
