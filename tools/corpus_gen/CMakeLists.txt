# corpus_gen: generator of the synthetic STEP and IGES parts in tests/corpus/files/raycad-synthetic/.
#
# A standalone project, not part of the RayCAD build (only src/kernel may link OpenCASCADE there).
# It needs the same OpenCASCADE as the product, from vcpkg. See tests/corpus/README.md for the commands.
cmake_minimum_required(VERSION 3.28)
project(raycad_corpus_gen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

find_package(OpenCASCADE CONFIG REQUIRED)

add_executable(corpus_gen corpus_gen.cpp)
target_include_directories(corpus_gen PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(corpus_gen PRIVATE
    TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKGeomAlgo TKTopAlgo TKPrim TKBO TKBool
    TKShHealing TKFillet TKOffset TKXSBase TKDE TKDESTEP TKDEIGES)
if(MSVC)
    target_compile_options(corpus_gen PRIVATE /W3 /EHsc /bigobj /utf-8)
    target_compile_definitions(corpus_gen PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()
