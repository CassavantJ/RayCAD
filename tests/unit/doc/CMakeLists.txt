# doc unit tests: canonical JSON, number formatting, expressions, snapshot serialisation (task T-008); commands,
# undo, previews and change listeners (task T-009); the rebuilder, the import feature and rebuild scheduling (T-010).
add_executable(ray_doc_tests
    commands_document_test.cpp
    commands_handlers_test.cpp
    commands_random_test.cpp
    expr_test.cpp
    hole_test.cpp
    json_test.cpp
    number_test.cpp
    parametric_test.cpp
    rebuild_document_test.cpp
    rebuild_import_test.cpp
    rebuild_reuse_test.cpp
    rebuild_test.cpp
    sketch_stability_test.cpp
    sketch_tool_commands_test.cpp
    sketch_dimension_commands_test.cpp
    sketch_solved_store_test.cpp
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

# The S4 part as document features judged by the T-081a naming oracle (task T-096). The oracle library and the
# harness and scenario library of tests/naming/oracle (ray_naming_harness) are linked here unchanged (not copied).
set(RAY_ORACLE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/tests/naming/oracle")
add_executable(ray_doc_s4_tests
    s4_document_test.cpp
    rebuild_bench_test.cpp)
target_link_libraries(ray_doc_s4_tests PRIVATE ray_naming_harness ray_naming_oracle ray_doc ray_naming ray_kernel
    ray_sketch ray_core Catch2::Catch2WithMain)
target_include_directories(ray_doc_s4_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/doc" "${RAY_ORACLE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/tests/unit/core_jobs")
target_compile_definitions(ray_doc_s4_tests PRIVATE RAY_ORACLE_DIR="${RAY_ORACLE_SOURCE_DIR}"
    RAY_GOLDEN_GEOMETRY_DIR="${CMAKE_SOURCE_DIR}/tests/golden/geometry"
    RAY_DOC_KERNEL_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/unit/kernel/data")
set_property(TARGET ray_doc_s4_tests PROPERTY FOLDER "tests")
add_test(NAME ray_doc_s4 COMMAND ray_doc_s4_tests "[s4doc]")
ray_test_runtime_path(ray_doc_s4)
set_tests_properties(ray_doc_s4 PROPERTIES TIMEOUT 3600)
add_test(NAME ray_doc_s4_revolve COMMAND ray_doc_s4_tests "[s4doc-revolve]")
ray_test_runtime_path(ray_doc_s4_revolve)
set_tests_properties(ray_doc_s4_revolve PROPERTIES TIMEOUT 3600)
# The S4 part with its hole sweeps made by the hole feature (task T-097).
add_test(NAME ray_doc_s4_hole COMMAND ray_doc_s4_tests "[s4doc-hole]")
ray_test_runtime_path(ray_doc_s4_hole)
set_tests_properties(ray_doc_s4_hole PROPERTIES TIMEOUT 3600)
# Incremental rebuild of a 100-feature part (PLAN.md section 9) and the geometry goldens (tests/golden/geometry).
add_test(NAME ray_doc_rebuild_bench COMMAND ray_doc_s4_tests "[rebuild-bench]")
ray_test_runtime_path(ray_doc_rebuild_bench)
set_tests_properties(ray_doc_rebuild_bench PROPERTIES TIMEOUT 1800)
add_test(NAME ray_doc_golden_geometry COMMAND ray_doc_s4_tests "[golden-geometry]")
ray_test_runtime_path(ray_doc_golden_geometry)
