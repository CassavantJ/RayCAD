# vcpkg overlay of the built-in x64-linux-dynamic triplet (every Linux preset but linux-clang-tsan; overlays are
# found through VCPKG_OVERLAY_TRIPLETS in CMakePresets.json and --overlay-triplets in CI). One change: the
# dependencies are compiled with -ffp-contract=off. Where the compiler's default target has FMA (the CI's GCC 15
# on ubuntu-26.04: a build for x86-64-v3 reproduces its results bit for bit), GCC fuses multiply-adds, and OpenCASCADE's
# geometry then differed in the last bits between code paths in one process: a shape and its serialised copy gave
# different lengths and areas, and the hole, naming and kernel goldens changed. RayCAD's own code is built the same
# way (CMakeLists.txt). PLAN.md section 8.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)

set(VCPKG_FIXUP_ELF_RPATH ON)

set(VCPKG_C_FLAGS "-ffp-contract=off")
set(VCPKG_CXX_FLAGS "-ffp-contract=off")
