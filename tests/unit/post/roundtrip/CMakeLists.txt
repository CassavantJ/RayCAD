# T-043: a test-only G-code interpreter and the round trip of every shipped post's goldens back to their toolpaths.
#
# ray_gcode_interp is the interpreter alone. It links nothing (the C++ standard library only) and has no include
# directory of src/post, so it cannot use the post host's writer, number formatting, arc or cycle code; the test
# "the interpreter shares no code with src/post" also checks its sources' includes. ray_roundtrip_compare adds the
# toolpath IR side (include/ray/cam.h through ray_cam). Only the test executable, which posts programs for the
# pairwise property combinations, links ray_post.
add_library(ray_gcode_interp STATIC gcode_interp.h gcode_interp.cpp)
set_property(TARGET ray_gcode_interp PROPERTY FOLDER "tests")

add_library(ray_roundtrip_compare STATIC roundtrip_compare.h roundtrip_compare.cpp)
target_link_libraries(ray_roundtrip_compare PUBLIC ray_gcode_interp ray_cam)
set_property(TARGET ray_roundtrip_compare PROPERTY FOLDER "tests")

add_executable(ray_post_roundtrip_tests roundtrip_test.cpp interp_test.cpp)
target_link_libraries(ray_post_roundtrip_tests PRIVATE ray_roundtrip_compare ray_gcode_interp ray_post ray_cam
    ray_core Catch2::Catch2WithMain)
# The shipped-post matrix (../shipped_matrix.h, ../post_test_util.h) and the post host's engine.h, for posting.
target_include_directories(ray_post_roundtrip_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/post" "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_definitions(ray_post_roundtrip_tests PRIVATE
    RAY_POST_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
    RAY_ROUNDTRIP_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    RAY_GOLDEN_GCODE_ROOT="${CMAKE_SOURCE_DIR}/tests/golden/gcode"
    RAY_TOOLPATH_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden/toolpaths"
    RAY_SHIPPED_POSTS_DIR="${CMAKE_SOURCE_DIR}/resources/library/posts")
ray_add_test(ray_post_roundtrip_tests)
