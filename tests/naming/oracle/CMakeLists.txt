# The naming oracle and scenario tests (task T-081a), promoted from spike S4.
#
# ray_naming_oracle is the oracle alone: it links ray_kernel and ray_core, NOT ray_naming, so it cannot use the
# naming code it judges. The harness (harness.h) and the tests link both.
add_library(ray_naming_oracle STATIC oracle.h oracle.cpp)
target_link_libraries(ray_naming_oracle PUBLIC ray_kernel ray_core)
set_property(TARGET ray_naming_oracle PROPERTY FOLDER "tests")

add_executable(ray_naming_oracle_tests
    harness.h harness.cpp
    s4_part.h s4_part.cpp
    s4_test.cpp
    random_test.cpp
    acceptance_test.cpp)
target_link_libraries(ray_naming_oracle_tests PRIVATE ray_naming_oracle ray_naming ray_kernel ray_core
    Catch2::Catch2WithMain)
target_compile_definitions(ray_naming_oracle_tests PRIVATE RAY_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus/files")
# shape_testing.h (private to src/kernel, no OCCT header): the count of OCCT faults the kernel guard caught.
target_include_directories(ray_naming_oracle_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/kernel")
set_property(TARGET ray_naming_oracle_tests PROPERTY FOLDER "tests")

# The scripted acceptance scenarios (seconds).
add_test(NAME ray_naming_oracle_tests COMMAND ray_naming_oracle_tests "[acceptance]")
ray_test_runtime_path(ray_naming_oracle_tests)
# The 47 S4 scenarios take about half a minute; their own CTest entry with its own timeout.
add_test(NAME ray_naming_s4 COMMAND ray_naming_oracle_tests "[s4]")
ray_test_runtime_path(ray_naming_s4)
set_tests_properties(ray_naming_s4 PROPERTIES TIMEOUT 3600)
# 1,000 randomised sequences on 20 corpus parts.
add_test(NAME ray_naming_random COMMAND ray_naming_oracle_tests "[random]")
ray_test_runtime_path(ray_naming_random)
set_tests_properties(ray_naming_random PROPERTIES TIMEOUT 7200)
