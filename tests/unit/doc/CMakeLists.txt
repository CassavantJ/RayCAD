# doc unit tests: canonical JSON, number formatting, expressions, snapshot serialisation (task T-008); commands,
# undo, previews and change listeners (task T-009); the rebuilder, the import feature and rebuild scheduling (T-010).
add_executable(ray_doc_tests
    commands_document_test.cpp
    commands_handlers_test.cpp
    commands_random_test.cpp
    expr_test.cpp
    json_test.cpp
    number_test.cpp
    parametric_test.cpp
    rebuild_document_test.cpp
    rebuild_import_test.cpp
    rebuild_test.cpp
    sketch_stability_test.cpp
    state_test.cpp
)
# The rebuild tests use kernel shapes and element maps directly (PLAN.md 1.1 lets doc depend on kernel and naming).
target_link_libraries(ray_doc_tests PRIVATE ray_doc ray_kernel ray_naming ray_sketch ray_core Catch2::Catch2WithMain)
# json.h, json_number.h, expr.h, document_impl.h, rebuild.h and features/ are private to src/doc; test_dispatcher.h
# is the UI-thread double.
target_include_directories(ray_doc_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/doc" "${CMAKE_SOURCE_DIR}/tests/unit/core_jobs")
# Golden snapshot files; tests/lint/check_doc_schema.py validates the same files against schemas/doc*.schema.json.
# The import tests read the small STEP and IGES files of the kernel tests.
target_compile_definitions(ray_doc_tests PRIVATE RAY_DOC_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
    RAY_DOC_KERNEL_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/unit/kernel/data")
ray_add_test(ray_doc_tests)
