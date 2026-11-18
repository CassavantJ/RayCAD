# store unit tests (tasks T-011, T-012): the .raycad container, objects, commits, lock file, kill and snapshot tests;
# the recovery journal, migrations and compaction.
# Hidden tags, run by hand: "[.kill500]" (500 kills during save), "[.bench500]" (500 MB file), "[.journal200]" (200
# killed journal sessions replayed), "[.compactkill100]" (100 kills during compaction), "[.journalbench]" (journal
# append under 2 ms median; the normal run allows 10 ms for slow CI disks).
find_package(unofficial-sqlite3 CONFIG REQUIRED)   # the tests look at files with plain SQLite

# The helper process the tests spawn and kill (store_helper.cpp).
add_executable(ray_store_helper store_helper.cpp store_test_util.cpp)
target_link_libraries(ray_store_helper PRIVATE ray_store ray_doc ray_core)
target_include_directories(ray_store_helper PRIVATE "${CMAKE_SOURCE_DIR}/src/store"
                                                    "${CMAKE_SOURCE_DIR}/tests/unit/core_jobs")
target_compile_definitions(ray_store_helper PRIVATE RAY_STORE_HELPER="$<TARGET_FILE:ray_store_helper>")
set_property(TARGET ray_store_helper PROPERTY FOLDER "tests")

add_executable(ray_store_tests
    bench_test.cpp
    compact_test.cpp
    file_test.cpp
    journal_kill_test.cpp
    journal_test.cpp
    kill_test.cpp
    lock_test.cpp
    migrate_test.cpp
    snapshot_test.cpp
    store_test_util.cpp
)
target_link_libraries(ray_store_tests PRIVATE ray_store ray_doc ray_core unofficial::sqlite3::sqlite3
                                              Catch2::Catch2WithMain)
# file.h, lock.h, journal.h, migrate.h and compact.h are private to src/store; test_dispatcher.h is the UI-thread
# double of the document tests.
target_include_directories(ray_store_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/store"
                                                   "${CMAKE_SOURCE_DIR}/tests/unit/core_jobs")
target_compile_definitions(ray_store_tests PRIVATE RAY_STORE_HELPER="$<TARGET_FILE:ray_store_helper>")
add_dependencies(ray_store_tests ray_store_helper)
ray_add_test(ray_store_tests)
