# store unit tests (task T-011): the .raycad container, objects, commits, lock file, kill and snapshot tests.
# Hidden tags, run by hand: "[.kill500]" (500 kills during save), "[.bench500]" (500 MB file).
find_package(unofficial-sqlite3 CONFIG REQUIRED)   # the tests look at files with plain SQLite

# The helper process the tests spawn and kill (store_helper.cpp).
add_executable(ray_store_helper store_helper.cpp store_test_util.cpp)
target_link_libraries(ray_store_helper PRIVATE ray_store ray_doc ray_core)
target_include_directories(ray_store_helper PRIVATE "${CMAKE_SOURCE_DIR}/src/store")
target_compile_definitions(ray_store_helper PRIVATE RAY_STORE_HELPER="$<TARGET_FILE:ray_store_helper>")
set_property(TARGET ray_store_helper PROPERTY FOLDER "tests")

add_executable(ray_store_tests
    bench_test.cpp
    file_test.cpp
    kill_test.cpp
    lock_test.cpp
    snapshot_test.cpp
    store_test_util.cpp
)
target_link_libraries(ray_store_tests PRIVATE ray_store ray_doc ray_core unofficial::sqlite3::sqlite3
                                              Catch2::Catch2WithMain)
# file.h and lock.h are private to src/store.
target_include_directories(ray_store_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/store")
target_compile_definitions(ray_store_tests PRIVATE RAY_STORE_HELPER="$<TARGET_FILE:ray_store_helper>")
add_dependencies(ray_store_tests ray_store_helper)
ray_add_test(ray_store_tests)
