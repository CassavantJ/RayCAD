# The adaptive roughing quality review pack (task T-062): review_pack_test.cpp rebuilds docs/reports/adaptive.md and
# its images in docs/reports/adaptive/ from the template report.md.in, the benchmark parts modelled with the kernel, the
# adaptive2d and pocket2d operations, the sweep checker, the cycle-time estimate and the starter library. The test
# fails when the committed report or an image differs from what it builds or a field is missing; it takes minutes in
# Release (tests/tiers.cmake: slow tier). Rebuild the files with:  ray_bench_adaptive_review "[write]"
add_executable(ray_bench_adaptive_review
    review_pack_test.cpp
    s5_reference.cpp
    s5_reference.h
    report.md.in
)
target_link_libraries(ray_bench_adaptive_review PRIVATE ray_sweepcheck ray_cam_invariants ray_cam ray_kernel ray_lib
    ray_core Catch2::Catch2WithMain)
target_include_directories(ray_bench_adaptive_review PRIVATE "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/tests/unit/cam")
target_compile_definitions(ray_bench_adaptive_review PRIVATE RAY_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    RAY_LIBRARY_DIR="${CMAKE_SOURCE_DIR}/resources/library")
set_property(TARGET ray_bench_adaptive_review PROPERTY FOLDER "tests")
add_test(NAME ray_bench_adaptive_review COMMAND ray_bench_adaptive_review "[adaptive_review]")
ray_test_runtime_path(ray_bench_adaptive_review)
set_tests_properties(ray_bench_adaptive_review PROPERTIES TIMEOUT 1800)
