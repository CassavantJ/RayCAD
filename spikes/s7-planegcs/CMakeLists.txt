cmake_minimum_required(VERSION 3.24)
project(s7_planegcs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(S7_OPENMP "Build with OpenMP so Eigen can use threads (determinism test only)" OFF)
option(S7_MPL2_ONLY "Define EIGEN_MPL2_ONLY: fails to compile if any non-MPL2 Eigen code is used" OFF)
option(S7_AVX2 "Build with AVX2 (determinism test only)" OFF)

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set(GCS_DIR ${TP}/freecad/src/Mod/Sketcher/App/planegcs)
set(SHIM ${CMAKE_CURRENT_SOURCE_DIR}/shim/include)

# PlaneGCS, unmodified sources.
add_library(planegcs STATIC
    ${GCS_DIR}/Constraints.cpp
    ${GCS_DIR}/GCS.cpp
    ${GCS_DIR}/Geo.cpp
    ${GCS_DIR}/SubSystem.cpp
    ${GCS_DIR}/qp_eq.cpp)
target_include_directories(planegcs PUBLIC ${GCS_DIR})
# shim/include          -> <Base/Console.h>, <FCConfig.h>, <boost_graph_adjacency_list.hpp>
# shim/include/_up/_up  -> makes "../../SketcherGlobal.h" resolve to shim/include/SketcherGlobal.h
target_include_directories(planegcs PUBLIC ${SHIM} ${SHIM}/_up/_up)
target_include_directories(planegcs SYSTEM PUBLIC ${TP}/eigen ${TP}/boost-subset)
if(S7_MPL2_ONLY)
    target_compile_definitions(planegcs PUBLIC EIGEN_MPL2_ONLY)
endif()
if(MSVC)
    target_compile_options(planegcs PUBLIC /utf-8 /bigobj /external:W0 PRIVATE /W3)
    target_compile_definitions(planegcs PUBLIC NOMINMAX _USE_MATH_DEFINES)
    if(S7_AVX2)
        target_compile_options(planegcs PUBLIC /arch:AVX2)
    endif()
    if(S7_OPENMP)
        target_compile_options(planegcs PUBLIC /openmp)
    endif()
else()
    find_package(Threads REQUIRED)   # GCS.cpp uses std::async
    target_link_libraries(planegcs PUBLIC Threads::Threads)
    if(S7_AVX2)
        target_compile_options(planegcs PUBLIC -mavx2 -mfma)
    endif()
    if(S7_OPENMP)
        find_package(OpenMP REQUIRED)
        target_link_libraries(planegcs PUBLIC OpenMP::OpenMP_CXX)
    endif()
endif()

add_executable(s7_bench src/bench.cpp)
target_link_libraries(s7_bench PRIVATE planegcs)
