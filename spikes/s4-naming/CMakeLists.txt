cmake_minimum_required(VERSION 3.25)
project(s4_naming CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(OpenCASCADE CONFIG REQUIRED)
add_executable(s4 main.cpp script.cpp kernelx.cpp naming.cpp truth.cpp probes.cpp)
target_include_directories(s4 PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(s4 PRIVATE
  TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKGeomAlgo TKTopAlgo TKPrim TKBO TKBool
  TKShHealing TKFillet TKOffset TKMesh)
if(MSVC)
  target_compile_options(s4 PRIVATE /W3 /EHsc /bigobj)
  target_compile_definitions(s4 PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX _USE_MATH_DEFINES)
endif()
