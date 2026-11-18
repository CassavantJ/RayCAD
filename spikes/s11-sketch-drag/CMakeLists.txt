cmake_minimum_required(VERSION 3.24)
project(s11_sketch_drag LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Third-party sources are fetched, not committed: see tools/fetch_third_party.sh. Same pins as S7.
set(S11_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party" CACHE PATH "folder with freecad/, eigen/, boost-subset/")
set(TP ${S11_THIRD_PARTY})
set(GCS_DIR ${TP}/freecad/src/Mod/Sketcher/App/planegcs)
# S7's shim, reused by path (4 small headers standing in for FreeCAD's Base/Console.h and friends).
set(SHIM ${CMAKE_CURRENT_SOURCE_DIR}/../s7-planegcs/shim/include)

if(NOT EXISTS ${GCS_DIR}/GCS.cpp)
    message(FATAL_ERROR "PlaneGCS sources not found under ${TP}; run tools/fetch_third_party.sh")
endif()

# PlaneGCS, unmodified sources, built exactly as in S7.
add_library(planegcs STATIC
    ${GCS_DIR}/Constraints.cpp
    ${GCS_DIR}/GCS.cpp
    ${GCS_DIR}/Geo.cpp
    ${GCS_DIR}/SubSystem.cpp
    ${GCS_DIR}/qp_eq.cpp)
target_include_directories(planegcs PUBLIC ${GCS_DIR})
target_include_directories(planegcs PUBLIC ${SHIM} ${SHIM}/_up/_up)
target_include_directories(planegcs SYSTEM PUBLIC ${TP}/eigen ${TP}/boost-subset)
if(MSVC)
    target_compile_options(planegcs PUBLIC /utf-8 /bigobj /external:W0 PRIVATE /W3)
    target_compile_definitions(planegcs PUBLIC NOMINMAX _USE_MATH_DEFINES)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(planegcs PUBLIC Threads::Threads)
endif()

add_executable(s11_bench src/bench.cpp src/sqp.cpp)
target_link_libraries(s11_bench PRIVATE planegcs)
