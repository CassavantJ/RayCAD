# RayCAD top-level build. See PLAN.md section 10.
#
# Rules this file and the module files keep (checked by tools/lint_cmake_deps.py):
#   - one static library per module, named ray_<module>, declared in src/<module>/CMakeLists.txt
#   - module dependencies are plain target_link_libraries(ray_<m> PRIVATE ray_<dep> ...) lines
#   - only src/kernel links OpenCASCADE; only src/ui and src/render link Qt
cmake_minimum_required(VERSION 3.28)

project(RayCAD VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
# No C++20 modules in this project: skip dependency scanning (it needs clang-scan-deps, which is not always installed).
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(RAY_BUILD_TESTS "Build the tests" ON)
option(RAY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(RAY_SANITIZE "" CACHE STRING "Sanitizers for Clang/GCC builds, e.g. address,undefined or thread")

# Every executable and DLL lands in one folder per configuration, so tests find their DLLs on Windows.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ---------- Global compiler settings ----------
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    if(RAY_WARNINGS_AS_ERRORS)
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(RAY_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
    # No multiply-add fusing anywhere, not only in the deterministic modules (ray_deterministic below): where the
    # target has FMA (arm64; x86-64-v3), GCC and Clang fuse by default, and the same geometry came out
    # bit-different on different code paths in one process (kernel round trip, hole hash, naming): the Linux CI
    # (GCC 15, ubuntu-26.04) failed so, and a GCC build for x86-64-v3 reproduces its results bit for bit. MSVC
    # does not fuse unless asked. The vcpkg overlay triplets (triplets/) build the dependencies, OpenCASCADE among
    # them, the same way.
    add_compile_options(-ffp-contract=off)
endif()
if(APPLE)
    # <AssertMacros.h> otherwise defines check(), verify() and require() as macros; core.h has CancelToken::check().
    add_compile_definitions(__ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES=0)
endif()

if(RAY_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "RAY_SANITIZE is only wired for Clang and GCC")
    endif()
    add_compile_options(-fsanitize=${RAY_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=${RAY_SANITIZE})
    # Single checks turned off for code in single third-party headers; the file says which and why. A compile-time
    # list rather than a runtime suppression file: UBSan cannot suppress checks built as non-recoverable.
    if(RAY_SANITIZE MATCHES "undefined" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options("-fsanitize-ignorelist=${CMAKE_SOURCE_DIR}/cmake/ubsan_ignorelist.txt")
    endif()
endif()

# Where the DLLs of the vcpkg packages are, for tests on Windows. The OCCT toolkits are delay-loaded, and the
# vcpkg toolchain only copies DLLs an executable imports eagerly, so they are not next to the executables.
set(RAY_VCPKG_BIN_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}$<$<CONFIG:Debug>:/debug>/bin")

# ---------- Packages of a different vcpkg tree ----------
# find_package() caches <Package>_DIR. When a build folder is reconfigured with another VCPKG_INSTALLED_DIR or
# triplet (the linux-clang-tsan preset moved to its own tree and triplet with an instrumented oneTBB), the cached
# paths would keep the old tree's packages: the TSan build then linked the uninstrumented libtbb and reported false
# races inside oneTBB's headers. Cached package folders from another vcpkg tree are therefore forgotten here, so
# they are found again in the current one.
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(_ray_vcpkg_tree "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/")
    get_cmake_property(_ray_cache_vars CACHE_VARIABLES)
    foreach(_var IN LISTS _ray_cache_vars)
        if(_var MATCHES "_DIR$" AND "${${_var}}" MATCHES "/vcpkg_installed[^/]*/")
            string(FIND "${${_var}}/" "${_ray_vcpkg_tree}" _at)
            if(NOT _at EQUAL 0)
                message(STATUS "Forgetting ${_var}=${${_var}}: not in ${_ray_vcpkg_tree}")
                unset(${_var} CACHE)
            endif()
        endif()
    endforeach()
endif()

# ---------- Shared pieces ----------
find_package(tl-expected CONFIG REQUIRED)

# The public headers. Every module links this PUBLIC; it carries no module dependency by itself.
add_library(ray_headers INTERFACE)
target_include_directories(ray_headers INTERFACE "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(ray_headers INTERFACE tl::expected)
target_compile_features(ray_headers INTERFACE cxx_std_20)
target_compile_definitions(ray_headers INTERFACE RAY_VERSION_STRING="${PROJECT_VERSION}")

# Floating-point flags for deterministic modules (PLAN.md section 8): no contraction (so no FMA fusing),
# no fast math. x86-64 never uses x87 for double arithmetic.
function(ray_deterministic target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /fp:strict)
    else()
        target_compile_options(${target} PRIVATE -ffp-contract=off -fno-fast-math)
    endif()
    set_property(TARGET ${target} PROPERTY RAY_DETERMINISTIC TRUE)
endfunction()

# Common settings of a module library. DETERMINISTIC adds the section 8 floating-point flags.
function(ray_module target)
    cmake_parse_arguments(ARG "DETERMINISTIC" "" "" ${ARGN})
    target_link_libraries(${target} PUBLIC ray_headers)
    set_property(TARGET ${target} PROPERTY FOLDER "modules")
    if(ARG_DETERMINISTIC)
        ray_deterministic(${target})
    endif()
endfunction()

# ---------- Modules (order follows PLAN.md section 1.1) ----------
add_subdirectory(src/core)
add_subdirectory(src/kernel)
add_subdirectory(src/naming)
add_subdirectory(src/sketch)
add_subdirectory(src/lib)
add_subdirectory(src/cam)
add_subdirectory(src/doc)
add_subdirectory(src/store)
add_subdirectory(src/sim)
add_subdirectory(src/post)
add_subdirectory(src/api)
add_subdirectory(src/render)
add_subdirectory(src/ui)

add_subdirectory(apps/raycad-cli)
add_subdirectory(apps/raycad)

if(RAY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Precompiled headers, once every target exists (cmake/RayPch.cmake says which targets and why).
include(cmake/RayPch.cmake)
ray_precompile_all()
