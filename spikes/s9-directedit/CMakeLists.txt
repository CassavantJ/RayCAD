cmake_minimum_required(VERSION 3.25)
project(s9_directedit CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(OpenCASCADE CONFIG REQUIRED)
add_executable(s9 main.cpp gen.cpp)
target_include_directories(s9 PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(s9 PRIVATE
  TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKGeomAlgo TKTopAlgo TKPrim TKBO TKBool
  TKShHealing TKFillet TKOffset TKFeat TKDESTEP TKXSBase TKDE)
if(MSVC)
  target_compile_options(s9 PRIVATE /W3 /EHsc /bigobj)
  target_compile_definitions(s9 PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()
