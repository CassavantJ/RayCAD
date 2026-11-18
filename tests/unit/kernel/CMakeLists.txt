# Kernel tests. occt_smoke_test.cpp calls into every toolkit ray_kernel links; on Windows it also proves that
# the toolkits are delay-loaded and that the set of DLLs loaded equals the list in src/kernel/CMakeLists.txt.
add_executable(ray_kernel_tests
    occt_smoke_test.cpp
    shape_test.cpp
    query_test.cpp)
target_link_libraries(ray_kernel_tests PRIVATE ray_kernel Catch2::Catch2WithMain)
# occt_smoke.h and shape_testing.h are private to src/kernel; they include no OCCT header.
target_include_directories(ray_kernel_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/kernel")
target_compile_definitions(ray_kernel_tests PRIVATE RAY_EXPECTED_OCCT_VERSION="8.0.1")
set_property(TARGET ray_kernel_tests PROPERTY FOLDER "tests")

# The smoke test needs a process in which no OCCT DLL has been loaded yet, and Catch2 runs test cases in
# random order, so it runs in a process of its own ([occt] tag) and everything else in another.
add_test(NAME ray_kernel_occt_smoke COMMAND ray_kernel_tests "[occt]")
add_test(NAME ray_kernel_tests COMMAND ray_kernel_tests "~[occt]")
ray_test_runtime_path(ray_kernel_occt_smoke)
ray_test_runtime_path(ray_kernel_tests)
