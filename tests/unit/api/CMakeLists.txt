# api unit tests (task T-017): the action registry, the app session, the headless app and crash recovery.

# The process the recovery tests kill (api_helper.cpp); the CLI recovery test uses it too.
add_executable(ray_api_helper api_helper.cpp)
target_link_libraries(ray_api_helper PRIVATE ray_api)
set_property(TARGET ray_api_helper PROPERTY FOLDER "tests")

add_executable(ray_api_tests
    actions_test.cpp
    external_app_test.cpp
    measure_test.cpp
    recovery_test.cpp
    session_test.cpp
    settings_test.cpp
    # Temp folders with non-ASCII names and child processes, shared with the store tests.
    "${CMAKE_SOURCE_DIR}/tests/unit/store/store_test_util.cpp"
)
# The tests reach the document, store and kernel types through <ray/api.h>; they link what the api links.
target_link_libraries(ray_api_tests PRIVATE ray_api ray_store ray_doc ray_kernel ray_core Catch2::Catch2WithMain)
# schema.h and json.h are private to src/api; test_dispatcher.h is the UI-thread double of the job tests;
# store_test_util.h and the store's private lock.h serve the process helpers.
target_include_directories(ray_api_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/api" "${CMAKE_SOURCE_DIR}/tests/unit/core_jobs"
    "${CMAKE_SOURCE_DIR}/tests/unit/store" "${CMAKE_SOURCE_DIR}/src/store"
    # The measure tests (T-022) build reference parts with the kernel's test builders (shape_testing.h, no OCCT header).
    "${CMAKE_SOURCE_DIR}/src/kernel")
target_compile_definitions(ray_api_tests PRIVATE RAY_KERNEL_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/unit/kernel/data"
    RAY_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus/files" RAY_STORE_HELPER="$<TARGET_FILE:ray_api_helper>")
add_dependencies(ray_api_tests ray_api_helper)
ray_add_test(ray_api_tests)
