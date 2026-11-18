# kernel: the only OCCT wrapper. This is the ONLY CMakeLists that may name OpenCASCADE (PLAN.md 0.4 rule 1).
find_package(OpenCASCADE CONFIG REQUIRED)

# The toolkits ray_kernel calls into directly. Keep this list and occt_smoke.cpp in step: the smoke test
# fails if a toolkit listed here is not exercised, or (on Windows) if one is exercised but not listed.
set(RAY_OCCT_TOOLKITS
    TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKGeomAlgo TKTopAlgo TKPrim
    TKBO TKBool TKShHealing TKFillet TKOffset TKFeat TKMesh TKHLR
    TKXSBase TKDE TKDESTEP TKDEIGES TKDESTL TKLCAF TKXCAF)

# Shape::contentHash and the serialisation checksum use BLAKE3 directly until ray::Hasher (T-002) is merged.
# TODO(T-002 merge): use ray::Hasher and drop this package and the BLAKE3::blake3 link below.
find_package(BLAKE3 CONFIG REQUIRED)

add_library(ray_kernel STATIC
    kernel_module.cpp occt_smoke.cpp occt_smoke.h
    shape_impl.h shape.cpp shape_guard.cpp shape_serialize.cpp shape_testing.h shape_testing.cpp
    query.cpp)
ray_module(ray_kernel DETERMINISTIC)
target_link_libraries(ray_kernel PRIVATE ray_core ${RAY_OCCT_TOOLKITS})
target_link_libraries(ray_kernel PRIVATE BLAKE3::blake3)
target_include_directories(ray_kernel SYSTEM PRIVATE ${OpenCASCADE_INCLUDE_DIR})
list(JOIN RAY_OCCT_TOOLKITS "," _ray_occt_toolkits_csv)
target_compile_definitions(ray_kernel PRIVATE RAY_OCCT_TOOLKITS="${_ray_occt_toolkits_csv}")

if(MSVC)
    # Delay-load every OCCT DLL (spike S2: about 170 ms saved before main() on a first launch). The flags
    # are INTERFACE because /DELAYLOAD belongs to the link of the final executable, not of this archive.
    # A missing DLL now fails at first use, not at start-up; tests/unit/kernel covers every toolkit.
    foreach(_tk IN LISTS RAY_OCCT_TOOLKITS)
        target_link_options(ray_kernel INTERFACE "/DELAYLOAD:${_tk}.dll")
    endforeach()
    # LNK4199: an executable that calls into no OCCT toolkit yet gets "/DELAYLOAD ignored" for each one.
    target_link_options(ray_kernel INTERFACE "/IGNORE:4199")
    target_link_libraries(ray_kernel INTERFACE delayimp)
endif()
