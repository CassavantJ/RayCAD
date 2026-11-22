# RayCAD overlay port of QuickJS-NG (vcpkg-configuration.json "overlay-ports"). The pinned vcpkg baseline has
# quickjs-ng 0.16.2; RayCAD needs 0.18.0 (PLAN.md section 2.1 and spike S8) with two patches, recorded in
# THIRD_PARTY.md:
#   0001  JS_INTERRUPT_COUNTER_INIT 100 instead of 10000 (the post host's CPU limit and cancel act through it)
#   0002  QJS_BUILD_PROGRAMS=OFF builds only the qjs library (no quickjs-libc, no qjs/qjsc executables), and
#         -ffp-contract=off on GCC and Clang (byte-identical output on every OS)
# Otherwise this follows the baseline port. port-version 1 marks the patched build in vcpkg output. The libc
# feature of the baseline port is not offered: RayCAD never links quickjs-libc.
if(VCPKG_TARGET_IS_WINDOWS)
    vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO quickjs-ng/quickjs
    REF "v${VERSION}"
    SHA512 8ed91ecca1a509dd67bdb57ca3fda237b1502710043dbbea1335a95e054e4bf1ef0c4c4eba7869d59e2b2bbf6faf2a1c1394a05f231ee78f6d981e9c5460cef0
    HEAD_REF master
    PATCHES
        0001-interrupt-counter-100.patch # RayCAD: see the header of the patch
        0002-library-only-no-fp-contract.patch # RayCAD: see the header of the patch
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DQJS_BUILD_LIBC=OFF
        -DQJS_BUILD_PROGRAMS=OFF
        -DQJS_BUILD_EXAMPLES=OFF
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/qjs PACKAGE_NAME qjs)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
# Upstream installs its examples folder and the licence under share/doc; the licence goes to the copyright file.
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/share/doc")

vcpkg_install_copyright(FILE_LIST
    "${SOURCE_PATH}/LICENSE"
    "${SOURCE_PATH}/libunicode-table.h"
)
