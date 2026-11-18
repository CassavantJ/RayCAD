# naming unit tests (task T-007): element maps for imported bodies, serialisation and the resolver.
add_executable(ray_naming_tests
    naming_test_util.h
    element_map_test.cpp
    name_imported_test.cpp
    resolver_test.cpp)
target_link_libraries(ray_naming_tests PRIVATE ray_naming ray_kernel ray_core Catch2::Catch2WithMain)
# The private headers of src/naming (test hooks) and src/kernel (shape_testing.h builders); neither includes an
# OCCT header.
target_include_directories(ray_naming_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/naming" "${CMAKE_SOURCE_DIR}/src/kernel")
ray_add_test(ray_naming_tests)
