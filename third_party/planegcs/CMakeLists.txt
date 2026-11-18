# PlaneGCS, FreeCAD's 2D constraint solver, vendored unmodified (see README.md). Added from src/sketch/CMakeLists.txt;
# only ray_sketch links ray_planegcs.
#
# Determinism (PLAN.md section 8, adapter rule 3): PlaneGCS is compiled with the same floating-point flags as the
# deterministic modules (ray_deterministic: /fp:strict on MSVC, -ffp-contract=off -fno-fast-math elsewhere) and with
# no SIMD flags beyond the platform default, so the solver and the adapter around it form one fixed set of flags.

# Third-party code: build it as upstream wrote it, without the project's warning flags (/W4 /WX would stop on
# upstream warnings). Sanitizer and other non-warning options stay.
get_property(_ray_options DIRECTORY PROPERTY COMPILE_OPTIONS)
list(FILTER _ray_options EXCLUDE REGEX "^(/W[0-4X]|/WX|-W.*|-Werror)$")
set_property(DIRECTORY PROPERTY COMPILE_OPTIONS ${_ray_options})

find_package(Eigen3 CONFIG REQUIRED)
find_package(boost_headers CONFIG REQUIRED)

add_library(ray_planegcs STATIC
    src/Constraints.cpp
    src/GCS.cpp
    src/Geo.cpp
    src/SubSystem.cpp
    src/qp_eq.cpp
)
set_target_properties(ray_planegcs PROPERTIES FOLDER "third_party")
# shim/include         -> <Base/Console.h>, <FCConfig.h>, <boost_graph_adjacency_list.hpp>
# shim/include/_up/_up -> makes "../../SketcherGlobal.h" (as GCS.h, Geo.h and Constraints.h include it) resolve to
#                         shim/include/SketcherGlobal.h without editing the sources.
# SYSTEM, so the project's /W4 /WX does not apply to warnings inside these headers where ray_sketch includes them.
target_include_directories(ray_planegcs SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/shim/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/shim/include/_up/_up")
# Header-only Boost (Graph's adjacency_list and connected_components, Math's constants); nothing is linked.
target_link_libraries(ray_planegcs PUBLIC Eigen3::Eigen Boost::headers)
# EIGEN_NO_DEBUG: FreeCAD builds PlaneGCS with it (an Eigen assertion in SparseQR fires on valid input, see the note
# at the top of GCS.cpp). PUBLIC, so every translation unit that instantiates Eigen code for the solver sees the
# same definition. BOOST_ALL_NO_LIB: no MSVC auto-linking; only headers are used.
target_compile_definitions(ray_planegcs PUBLIC EIGEN_NO_DEBUG BOOST_ALL_NO_LIB)
if(MSVC)
    target_compile_definitions(ray_planegcs PUBLIC _USE_MATH_DEFINES)
    target_compile_options(ray_planegcs PRIVATE /W3 /bigobj /fp:strict)
else()
    target_compile_options(ray_planegcs PRIVATE -w -ffp-contract=off -fno-fast-math)
    # GCS::System::diagnose runs one of its two QR decompositions through std::async.
    find_package(Threads REQUIRED)
    target_link_libraries(ray_planegcs PUBLIC Threads::Threads)
endif()
target_compile_features(ray_planegcs PUBLIC cxx_std_20)
