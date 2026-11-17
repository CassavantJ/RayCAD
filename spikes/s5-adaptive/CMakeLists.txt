cmake_minimum_required(VERSION 3.24)
project(s5_adaptive CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/EHsc /utf-8 /fp:precise)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES NOMINMAX)
endif()
set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# Clipper2 2.0.1 built with USINGZ (FreeCAD main links Clipper2::Clipper2Z)
set(C2 ${TP}/clipper2/CPP/Clipper2Lib)
add_library(clipper2z STATIC ${C2}/src/clipper.engine.cpp ${C2}/src/clipper.offset.cpp ${C2}/src/clipper.rectclip.cpp)
target_include_directories(clipper2z PUBLIC ${C2}/include)
target_compile_definitions(clipper2z PUBLIC USINGZ)

# Variant A: FreeCAD main Adaptive.cpp against Clipper2 2.0.1
add_executable(s5_gen_main src/gen.cpp ${TP}/freecad-main/Adaptive.cpp)
target_include_directories(s5_gen_main PRIVATE ${TP}/freecad-main src)
target_compile_definitions(s5_gen_main PRIVATE S5_VARIANT_MAIN)
target_link_libraries(s5_gen_main PRIVATE clipper2z)

# Variant B: FreeCAD 1.1.4 Adaptive.cpp with its bundled Clipper 6.4.2
add_executable(s5_gen_114 src/gen.cpp ${TP}/freecad/Adaptive.cpp ${TP}/freecad/clipper.cpp)
target_include_directories(s5_gen_114 PRIVATE ${TP}/freecad src)

# Independent checker: no Adaptive, no Clipper
add_executable(s5_check src/check.cpp)
target_include_directories(s5_check PRIVATE src)
