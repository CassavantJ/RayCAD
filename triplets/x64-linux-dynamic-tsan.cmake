# vcpkg overlay triplet for the linux-clang-tsan preset (selected in CMakePresets.json; no other preset
# uses it). It is x64-linux-dynamic (the overlay here, with -ffp-contract=off) with two changes:
#
# - oneTBB is built by Clang with ThreadSanitizer (oneTBB's own TBB_SANITIZE option). An uninstrumented
#   libtbb hides its synchronisation from TSan, which then reports races inside TBB's inlined header code
#   (blocked_range, start_for, fold_tree, the small-object pool) on every parallel loop. Clang matches
#   the compiler of the preset, so the library and the tests use the same TSan runtime. -g gives line
#   numbers in reports that pass through TBB. oneTBB adds the GCC-only -Wno-tsan with TBB_SANITIZE=thread
#   and then rejects Clang because Clang warns about that flag; -Wno-unknown-warning-option avoids that.
# - Only release libraries are built: the preset is single-config RelWithDebInfo, which uses them, and
#   this halves the one-off build of every dependency for this triplet (OpenCASCADE takes the longest).
#
# The other ports are built as for x64-linux-dynamic (GCC, no instrumentation).
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_FIXUP_ELF_RPATH ON)
set(VCPKG_BUILD_TYPE release)

# As x64-linux-dynamic.cmake: no multiply-add fusing.
set(VCPKG_C_FLAGS "-ffp-contract=off")
set(VCPKG_CXX_FLAGS "-ffp-contract=off")
if(PORT STREQUAL "tbb")
    set(VCPKG_C_FLAGS "-ffp-contract=off -g -Wno-unknown-warning-option")
    set(VCPKG_CXX_FLAGS "-ffp-contract=off -g -Wno-unknown-warning-option")
    set(VCPKG_CMAKE_CONFIGURE_OPTIONS
        -DCMAKE_C_COMPILER=clang
        -DCMAKE_CXX_COMPILER=clang++
        -DTBB_SANITIZE=thread
    )
endif()
