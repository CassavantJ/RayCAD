# api unit tests (task T-017): the action registry, the app session and the headless app.
add_executable(ray_api_tests
    actions_test.cpp
    external_app_test.cpp
    session_test.cpp
)
# The tests reach the document, store and kernel types through <ray/api.h>; they link what the api links.
target_link_libraries(ray_api_tests PRIVATE ray_api ray_store ray_doc ray_kernel ray_core Catch2::Catch2WithMain)
# schema.h and json.h are private to src/api; test_dispatcher.h is the UI-thread double of the job tests.
target_include_directories(ray_api_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/api" "${CMAKE_SOURCE_DIR}/tests/unit/core_jobs")
target_compile_definitions(ray_api_tests PRIVATE RAY_KERNEL_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/unit/kernel/data"
    RAY_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus/files")
ray_add_test(ray_api_tests)
