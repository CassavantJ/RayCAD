# The toolpath invariant checker (task T-058; README.md in this folder). A test-only library that shares no code with
# the generators: it links ray_cam for the toolpath IR and the operation registry, ray_kernel to mesh the part and to
# build the property tests' random parts, and nothing of src/cam's geometry (geom2d, link, the operations' internals).
add_library(ray_cam_invariants STATIC
    checker.cpp
    checker.h
    cycle_invariants.h    # the canned-cycle rules (T-035), run by checker.cpp
    mini_json.h
    motion.cpp
    motion.h
    part_model.cpp
    part_model.h
    property.cpp
    property.h
    reference_paths.cpp
    reference_paths.h
    stock_grid.cpp
    stock_grid.h
    tool_profile.cpp
    tool_profile.h
    toolpath_invariants.h # the general M2 rules (T-035), run by checker.cpp
)
target_include_directories(ray_cam_invariants PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ray_cam_invariants PUBLIC ray_cam ray_kernel ray_lib ray_core Catch2::Catch2)
set_property(TARGET ray_cam_invariants PROPERTY FOLDER "tests")

# Each invariant caught on a broken toolpath and passed on its correct twin, near misses at the error bounds, the
# reference generators, and the property tests of the operation types (one property_<type>_test.cpp per type).
add_executable(ray_cam_invariants_tests
    checker_geometry_test.cpp
    checker_moves_test.cpp
    checker_reference_test.cpp
    checker_test_util.h
    property_drill_test.cpp
    property_face_test.cpp # T-052
    property_contour2d_test.cpp # T-053
    property_harness_test.cpp
    property_pocket2d_test.cpp # T-054
)
target_link_libraries(ray_cam_invariants_tests PRIVATE ray_cam_invariants Catch2::Catch2WithMain)
set_property(TARGET ray_cam_invariants_tests PROPERTY FOLDER "tests")
# Three CTest tests of the one executable: the checker's own cases, the property tests on their quick seeds (both in
# the quick tier), and the property tests on their larger seed sets (the [.slow] cases; the slow tier).
add_test(NAME ray_cam_invariants_tests COMMAND ray_cam_invariants_tests "~[property]")
ray_test_runtime_path(ray_cam_invariants_tests)
add_test(NAME ray_cam_invariants_property COMMAND ray_cam_invariants_tests "[property]~[slow]")
ray_test_runtime_path(ray_cam_invariants_property)
add_test(NAME ray_cam_invariants_slow COMMAND ray_cam_invariants_tests "[slow]")
ray_test_runtime_path(ray_cam_invariants_slow)
# With an operation type more each (T-052 on): 200 seeds per type pass the presets' 300 s in Release, the 12 quick
# seeds per type come near it in an MSVC Debug build (about 300 s with drill, the reference type and face).
set_tests_properties(ray_cam_invariants_slow PROPERTIES TIMEOUT 3600)
set_tests_properties(ray_cam_invariants_property PROPERTIES TIMEOUT 1200)
