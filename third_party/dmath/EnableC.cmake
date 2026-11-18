# Enables the C language for the vendored dmath sources (see CMakeLists.txt in this folder). Included from
# src/core/CMakeLists.txt: C must be enabled in the directory of ray_core, which compiles the C objects in.
#
# Use the C compiler that belongs to the C++ compiler, so that a Clang build does not compile these files with
# the system gcc (sanitizer runtimes and flags must match). A compiler named by CC or CMAKE_C_COMPILER wins.
if(NOT CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
    get_filename_component(_ray_cxx_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_ray_cxx_name "${CMAKE_CXX_COMPILER}" NAME)
    set(_ray_c_name "")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        set(_ray_c_name "${_ray_cxx_name}")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND _ray_cxx_name MATCHES "clang\\+\\+")
        string(REPLACE "clang++" "clang" _ray_c_name "${_ray_cxx_name}")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND _ray_cxx_name MATCHES "g\\+\\+")
        string(REPLACE "g++" "gcc" _ray_c_name "${_ray_cxx_name}")
    endif()
    if(_ray_c_name AND EXISTS "${_ray_cxx_dir}/${_ray_c_name}")
        set(CMAKE_C_COMPILER "${_ray_cxx_dir}/${_ray_c_name}")
    endif()
endif()
enable_language(C)
