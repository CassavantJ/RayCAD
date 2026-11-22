# cam unit tests: the CAM state, the cam.* commands and the staleness engine (task T-032).
add_executable(ray_cam_engine_tests
    engine_api_test.cpp
    engine_document_test.cpp
    engine_test.cpp
)
# The engine reads document snapshots (doc::camSnapshot), the document tests rebuild parts with the doc tests' builders
# (tests/unit/doc), and the API test runs the commands as actions, so this links what the api links.
target_link_libraries(ray_cam_engine_tests PRIVATE ray_api ray_store ray_doc ray_cam ray_naming ray_kernel ray_core
    Catch2::Catch2WithMain)
# src/doc for its private JSON and the CAM codec (commands/cam_json.h), tests/unit/doc for its test builders,
# tests/unit/core_jobs for the UI-thread double, src/api for its schema validator (schema.h; listed last, as its
# json.h would otherwise shadow the doc one for the doc test headers).
target_include_directories(ray_cam_engine_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/doc" "${CMAKE_SOURCE_DIR}/tests/unit/doc"
    "${CMAKE_SOURCE_DIR}/tests/unit/core_jobs" "${CMAKE_SOURCE_DIR}/src/api")
target_compile_definitions(ray_cam_engine_tests PRIVATE RAY_DOC_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/unit/doc/golden"
    RAY_SCHEMA_DIR="${CMAKE_SOURCE_DIR}/schemas" RAY_DOC_KERNEL_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/unit/kernel/data")
ray_add_test(ray_cam_engine_tests)

# Setup resolution (task T-033): on hand-made builds, on a corpus part imported through the document, and in a document
# rebuilt by the real rebuilder through edits, undo and redo. Same links and include folders as the engine tests.
add_executable(ray_cam_setup_tests
    setup_document_test.cpp
    setup_test.cpp
)
target_link_libraries(ray_cam_setup_tests PRIVATE ray_api ray_store ray_doc ray_cam ray_naming ray_kernel ray_core
    Catch2::Catch2WithMain)
target_include_directories(ray_cam_setup_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/doc" "${CMAKE_SOURCE_DIR}/tests/unit/doc"
    "${CMAKE_SOURCE_DIR}/tests/unit/core_jobs")
target_compile_definitions(ray_cam_setup_tests PRIVATE RAY_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus/files"
    RAY_DOC_KERNEL_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/unit/kernel/data")
ray_add_test(ray_cam_setup_tests)
# Hole recognition and ordering (task T-034): the labelled corpus parts (holes_labels.txt), parts built through the
# kernel for each kind and each non-hole, hole features in a rebuilt document (thread pickup), and the drilling order.
# Same links and include folders as the setup tests.
add_executable(ray_cam_holes_tests
    holes_document_test.cpp
    holes_kernel_test.cpp
    holes_order_test.cpp
    holes_test.cpp
    holes_test_util.h
)
target_link_libraries(ray_cam_holes_tests PRIVATE ray_api ray_store ray_doc ray_cam ray_naming ray_kernel ray_core
    Catch2::Catch2WithMain)
target_include_directories(ray_cam_holes_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/doc" "${CMAKE_SOURCE_DIR}/tests/unit/doc"
    "${CMAKE_SOURCE_DIR}/tests/unit/core_jobs")
target_compile_definitions(ray_cam_holes_tests PRIVATE RAY_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus/files"
    RAY_HOLES_LABELS="${CMAKE_CURRENT_SOURCE_DIR}/holes_labels.txt"
    RAY_DOC_KERNEL_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/unit/kernel/data")
set_property(TARGET ray_cam_holes_tests PROPERTY FOLDER "tests")
# The timing cases ([wallclock]: a plate of a few thousand holes, ordering thousands of points) run alone, in the
# split tests/tiers.cmake makes.
add_test(NAME ray_cam_holes_tests COMMAND ray_cam_holes_tests "~[wallclock]")
ray_test_runtime_path(ray_cam_holes_tests)
# The toolpath IR: the RCTP format (Toolpath::serialize) and the canned-cycle rules of the toolpath invariant checker
# (invariants/, where T-058 adds the rest of the checker).
add_executable(ray_cam_toolpath_tests
    toolpath_format_test.cpp
    invariants/cycle_invariants.h
    invariants/cycle_invariants_test.cpp
    invariants/toolpath_invariants.h
    invariants/toolpath_invariants_test.cpp
)
target_link_libraries(ray_cam_toolpath_tests PRIVATE ray_cam ray_core Catch2::Catch2WithMain)
ray_add_test(ray_cam_toolpath_tests)
# Feeds and speeds (task T-048): computeFeeds on hand-made tools and machines, and on the shipped library
# (resources/library, read through ILibrary).
add_executable(ray_cam_feeds_tests feeds_test.cpp)
target_link_libraries(ray_cam_feeds_tests PRIVATE ray_cam ray_lib ray_core Catch2::Catch2WithMain)
target_compile_definitions(ray_cam_feeds_tests PRIVATE RAY_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
ray_add_test(ray_cam_feeds_tests)
# The drilling operation (task T-035): depths, feeds and checks on kernel-built parts, the golden toolpath of each cycle
# (tests/golden/toolpaths, with a corpus part) and the chain through the backplot and the post host to G-code, and end
# to end through the document and the engine. src/ for the type's private header (cam/ops/drill.h), the rest as the
# hole recognition tests.
add_executable(ray_cam_drill_tests
    drill_depth_test.cpp
    drill_document_test.cpp
    drill_golden_test.cpp
    drill_test_util.h
    drill_validate_test.cpp
    toolpath_dump.h
    invariants/toolpath_invariants.h
)
target_link_libraries(ray_cam_drill_tests PRIVATE ray_api ray_store ray_doc ray_cam ray_sim ray_post ray_naming
    ray_kernel ray_lib ray_core Catch2::Catch2WithMain)
target_include_directories(ray_cam_drill_tests PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/doc"
    "${CMAKE_SOURCE_DIR}/tests/unit/doc" "${CMAKE_SOURCE_DIR}/tests/unit/core_jobs")
target_compile_definitions(ray_cam_drill_tests PRIVATE RAY_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus/files"
    RAY_TOOLPATH_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden/toolpaths"
    RAY_POST_TEST_DIR="${CMAKE_SOURCE_DIR}/tests/unit/post" RAY_SCHEMA_DIR="${CMAKE_SOURCE_DIR}/schemas"
    RAY_DOC_KERNEL_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/unit/kernel/data")
ray_add_test(ray_cam_drill_tests)
