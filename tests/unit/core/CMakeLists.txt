# core unit tests (task T-002): identity, hashing, units, dmath, logging.
add_executable(ray_core_tests
    dmath_test.cpp
    hash_test.cpp
    identity_test.cpp
    log_test.cpp
    units_test.cpp
)
target_link_libraries(ray_core_tests PRIVATE ray_core Catch2::Catch2WithMain)
# log_sink.h is private to src/core.
target_include_directories(ray_core_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/core")
# The dmath reference table, written by tools/gen_dmath_table.py.
target_compile_definitions(ray_core_tests PRIVATE RAY_DMATH_TABLE="${CMAKE_CURRENT_SOURCE_DIR}/dmath_table.bin")
ray_add_test(ray_core_tests)
