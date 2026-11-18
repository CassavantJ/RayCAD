# core: Ids, hashing, Result, units, deterministic math, job system, logging. Allowed dependencies: PLAN.md section 1.1.
add_library(ray_core STATIC
    core_module.cpp
    dmath.cpp
    hash.cpp
    identity.cpp
    log.cpp
    units.cpp
)
ray_module(ray_core DETERMINISTIC)

# BLAKE3 for ray::Hasher.
find_package(BLAKE3 CONFIG REQUIRED)
target_link_libraries(ray_core PRIVATE BLAKE3::blake3)

# The vendored musl libm subset behind ray::dmath, compiled into this library. Its sources are C.
include("${PROJECT_SOURCE_DIR}/third_party/dmath/EnableC.cmake")
add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/dmath" "${PROJECT_BINARY_DIR}/third_party/dmath")
target_sources(ray_core PRIVATE $<TARGET_OBJECTS:dmath_musl>)
target_include_directories(ray_core PRIVATE "${PROJECT_SOURCE_DIR}/third_party/dmath/include")
