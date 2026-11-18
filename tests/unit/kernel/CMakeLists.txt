# OCCT smoke test: calls into every toolkit ray_kernel links. On Windows it also proves that the toolkits
# are delay-loaded and that the set of DLLs loaded equals the list in src/kernel/CMakeLists.txt.
add_executable(ray_kernel_tests occt_smoke_test.cpp)
target_link_libraries(ray_kernel_tests PRIVATE ray_kernel Catch2::Catch2WithMain)
# occt_smoke.h is private to src/kernel; it includes no OCCT header.
target_include_directories(ray_kernel_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/kernel")
target_compile_definitions(ray_kernel_tests PRIVATE RAY_EXPECTED_OCCT_VERSION="8.0.1")
ray_add_test(ray_kernel_tests)
