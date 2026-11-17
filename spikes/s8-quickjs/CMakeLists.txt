cmake_minimum_required(VERSION 3.20)
project(s8_quickjs C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# QuickJS-NG, pinned in third_party/ (see REPORT.md). Only the `qjs` static library is built;
# qjs.exe, qjsc, quickjs-libc (std/os modules) and the tests are EXCLUDE_FROM_ALL and never linked.
set(QJS_BUILD_LIBC OFF CACHE BOOL "" FORCE)
set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/quickjs EXCLUDE_FROM_ALL)

add_executable(s8host host.cpp)
target_link_libraries(s8host PRIVATE qjs)
if(MSVC)
    target_compile_options(s8host PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(s8host PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()
