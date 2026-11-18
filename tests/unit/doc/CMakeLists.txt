# doc unit tests: canonical JSON, number formatting, expressions, snapshot serialisation (task T-008); commands,
# undo, previews and change listeners (task T-009).
add_executable(ray_doc_tests
    commands_document_test.cpp
    commands_handlers_test.cpp
    commands_random_test.cpp
    expr_test.cpp
    json_test.cpp
    number_test.cpp
    state_test.cpp
)
target_link_libraries(ray_doc_tests PRIVATE ray_doc Catch2::Catch2WithMain)
# json.h, json_number.h, expr.h and document_impl.h are private to src/doc; test_dispatcher.h is the UI-thread double.
target_include_directories(ray_doc_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/doc" "${CMAKE_SOURCE_DIR}/tests/unit/core_jobs")
# Golden snapshot files; tests/lint/check_doc_schema.py validates the same files against schemas/doc*.schema.json.
target_compile_definitions(ray_doc_tests PRIVATE RAY_DOC_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
ray_add_test(ray_doc_tests)
