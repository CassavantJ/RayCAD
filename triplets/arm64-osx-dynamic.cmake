# vcpkg overlay of the built-in arm64-osx-dynamic triplet (preset macos-arm64). One change: the dependencies are
# compiled with -ffp-contract=off. arm64 has fused multiply-add and Apple Clang fuses within an expression by
# default, which makes OpenCASCADE's geometry differ in the last bits between code paths in one process (see
# x64-linux-dynamic.cmake here, where that was seen). RayCAD's own code is built the same way (CMakeLists.txt).
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)

set(VCPKG_C_FLAGS "-ffp-contract=off")
set(VCPKG_CXX_FLAGS "-ffp-contract=off")
