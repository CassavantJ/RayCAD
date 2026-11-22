# Post host tests (task T-039): number formatting, the writer, the host's motion services, the QuickJS-NG sandbox and
# its limits, the two test posts (posts/) with their goldens (tests/golden/gcode/host), and the 1 M-move bench.
add_executable(ray_post_tests
    post_test_util.h
    format_test.cpp
    writer_test.cpp
    motion_test.cpp
    sandbox_test.cpp
    engine_test.cpp
    host_additions_test.cpp
    shipped_posts_test.cpp
    bench_test.cpp)
# ray_cam for Toolpath::deserialize: the shipped-post matrix (T-040, T-041) posts the drilling operation's golden
# toolpaths (tests/golden/toolpaths).
target_link_libraries(ray_post_tests PRIVATE ray_post ray_cam ray_core Catch2::Catch2WithMain)
# The private headers of src/post (engine.h, engine_motion.h, writer.h, format.h); none includes quickjs.h.
target_include_directories(ray_post_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/post")
target_compile_definitions(ray_post_tests PRIVATE
    RAY_POST_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    RAY_GOLDEN_GCODE_DIR="${CMAKE_SOURCE_DIR}/tests/golden/gcode/host"
    RAY_GOLDEN_GCODE_ROOT="${CMAKE_SOURCE_DIR}/tests/golden/gcode"
    RAY_TOOLPATH_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden/toolpaths"
    RAY_SHIPPED_POSTS_DIR="${CMAKE_SOURCE_DIR}/resources/library/posts")
if(RAY_SANITIZE)
    target_compile_definitions(ray_post_tests PRIVATE RAY_SANITIZED_BUILD)
endif()
set_property(TARGET ray_post_tests PROPERTY FOLDER "tests")
# The bench ([bench][wallclock]) runs alone as ray_post_tests_wallclock (tests/tiers.cmake).
add_test(NAME ray_post_tests COMMAND ray_post_tests "~[wallclock]")
ray_test_runtime_path(ray_post_tests)
