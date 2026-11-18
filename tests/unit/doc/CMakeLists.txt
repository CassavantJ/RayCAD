# doc unit tests (task T-008): canonical JSON, number formatting, expressions, snapshot serialisation.
add_executable(ray_doc_tests
    expr_test.cpp
    json_test.cpp
    number_test.cpp
    state_test.cpp
)
target_link_libraries(ray_doc_tests PRIVATE ray_doc Catch2::Catch2WithMain)
# json.h, json_number.h and expr.h are private to src/doc.
target_include_directories(ray_doc_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/doc")
# Golden snapshot files; tests/lint/check_doc_schema.py validates the same files against schemas/doc*.schema.json.
target_compile_definitions(ray_doc_tests PRIVATE RAY_DOC_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
ray_add_test(ray_doc_tests)
