cmake_minimum_required(VERSION 3.25)
project(s3_step LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCASCADE CONFIG REQUIRED)

set(OCCT_LIBS
    TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKGeomAlgo TKTopAlgo TKPrim
    TKBO TKBool TKFillet TKOffset TKShHealing TKMesh TKXSBase TKDE TKDESTEP)

add_executable(s3gen gen.cpp)
target_include_directories(s3gen PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(s3gen PRIVATE ${OCCT_LIBS})

add_executable(s3bench bench.cpp)
target_include_directories(s3bench PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(s3bench PRIVATE ${OCCT_LIBS} psapi dbghelp)

if(MSVC)
    target_compile_options(s3gen PRIVATE /W3 /EHsc /utf-8)
    target_compile_options(s3bench PRIVATE /W3 /EHsc /utf-8)
    target_compile_definitions(s3gen PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(s3bench PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()
