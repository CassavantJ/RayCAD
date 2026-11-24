# sim unit tests: backplot (task T-037); tool shapes and the tri-dexel stock model (task T-070); safety checks (T-071);
# the simulator (T-072).
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
    # T-071: the safety checks.
    verify_test_util.h
    verify_geom_test.cpp
    verify_test.cpp
    verify_tier_test.cpp
    verify_golden_test.cpp
    verify_bench_test.cpp
    # T-072: the simulator, keyframes and gouges.
    simulator_test_util.h
    simulator_test.cpp
    simulator_determinism_test.cpp
    simulator_bench_test.cpp
    # The toolpath invariant checker's tool model (T-058), which makeToolShape must agree with.
    ${CMAKE_SOURCE_DIR}/tests/unit/cam/invariants/tool_profile.cpp)
target_link_libraries(ray_sim_tests PRIVATE ray_sim ray_core ray_kernel Catch2::Catch2WithMain)
# The private headers of src/sim (backplot.h: limits and test hooks).
target_include_directories(ray_sim_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/sim"
    "${CMAKE_SOURCE_DIR}/tests/unit/cam/invariants")
set_property(TARGET ray_sim_tests PROPERTY FOLDER "tests")
# T-071: the generated golden toolpaths the safety checks run over (verify_golden_test.cpp).
target_compile_definitions(ray_sim_tests PRIVATE RAY_TOOLPATH_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden/toolpaths")
# The [wallclock] benchmark runs alone as ray_sim_tests_wallclock (tests/tiers.cmake).
add_test(NAME ray_sim_tests COMMAND ray_sim_tests "~[wallclock]")
ray_test_runtime_path(ray_sim_tests)
# T-072: the simulator's section 9 benchmark (hidden from the suite) runs alone as its own test (tests/tiers.cmake).
add_test(NAME ray_sim_simulator_bench COMMAND ray_sim_tests "[simulator-bench]")
ray_test_runtime_path(ray_sim_simulator_bench)
