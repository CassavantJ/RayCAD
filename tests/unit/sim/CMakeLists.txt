# sim unit tests: backplot (task T-037); tool shapes and the tri-dexel stock model (task T-070).
add_executable(ray_sim_tests
    backplot_test_util.h
    backplot_test.cpp
    backplot_property_test.cpp
    backplot_golden_test.cpp
    backplot_bench_test.cpp
    tridexel_test_util.h
    toolshape_test.cpp
    tridexel_volume_test.cpp
    tridexel_stock_test.cpp
    tridexel_mesh_test.cpp
    tridexel_determinism_test.cpp
    tridexel_bench_test.cpp
    # The toolpath invariant checker's tool model (T-058), which makeToolShape must agree with.
    ${CMAKE_SOURCE_DIR}/tests/unit/cam/invariants/tool_profile.cpp)
target_link_libraries(ray_sim_tests PRIVATE ray_sim ray_core ray_kernel Catch2::Catch2WithMain)
# The private headers of src/sim (backplot.h: limits and test hooks).
target_include_directories(ray_sim_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/sim"
    "${CMAKE_SOURCE_DIR}/tests/unit/cam/invariants")
set_property(TARGET ray_sim_tests PROPERTY FOLDER "tests")
# The [wallclock] benchmark runs alone as ray_sim_tests_wallclock (tests/tiers.cmake).
add_test(NAME ray_sim_tests COMMAND ray_sim_tests "~[wallclock]")
ray_test_runtime_path(ray_sim_tests)
