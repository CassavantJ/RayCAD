# cam unit tests: the CAM state, the cam.* commands and the staleness engine (task T-032).
add_executable(ray_cam_engine_tests
    engine_api_test.cpp
    engine_document_test.cpp
    engine_notify_test.cpp # notifications, status on a worker, validate, previews, material and feeds record (T-045)
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
# The facing operation (task T-052): levels, strokes and refusals against hand-computed values, the golden toolpaths
# (tests/golden/toolpaths/face_*, with a corpus part) with the invariant checker (invariants/) and the coverage check by
# 2D sweep, the chain to G-code, and end to end through the document and the engine. src/ for the private headers.
add_executable(ray_cam_face_tests
    face_document_test.cpp
    face_golden_test.cpp
    face_plan_test.cpp
    face_test_util.h
    toolpath_dump.h
)
target_link_libraries(ray_cam_face_tests PRIVATE ray_cam_invariants ray_api ray_store ray_doc ray_cam ray_sim ray_post
    ray_naming ray_kernel ray_lib ray_core Catch2::Catch2WithMain)
target_include_directories(ray_cam_face_tests PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/doc"
    "${CMAKE_SOURCE_DIR}/tests/unit/doc" "${CMAKE_SOURCE_DIR}/tests/unit/core_jobs")
target_compile_definitions(ray_cam_face_tests PRIVATE RAY_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus/files"
    RAY_TOOLPATH_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden/toolpaths"
    RAY_POST_TEST_DIR="${CMAKE_SOURCE_DIR}/tests/unit/post" RAY_SCHEMA_DIR="${CMAKE_SOURCE_DIR}/schemas"
    RAY_DOC_KERNEL_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/unit/kernel/data")
ray_add_test(ray_cam_face_tests)
# The 2D contour operation (task T-053): offsets, sides, leads, passes, tabs and compensation against hand-computed
# values, each refusal, the goldens (tests/golden/toolpaths/contour2d_*) with the invariant checker on each, the chain
# to G-code, and end to end through the document and the engine. Links as the drill tests, plus the checker.
add_executable(ray_cam_contour2d_tests
    contour2d_document_test.cpp
    contour2d_golden_test.cpp
    contour2d_rules_test.cpp
    contour2d_test_util.h
    contour2d_validate_test.cpp
    toolpath_dump.h
)
target_link_libraries(ray_cam_contour2d_tests PRIVATE ray_cam_invariants ray_api ray_store ray_doc ray_cam ray_sim
    ray_post ray_naming ray_kernel ray_lib ray_core Catch2::Catch2WithMain)
target_include_directories(ray_cam_contour2d_tests PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/doc"
    "${CMAKE_SOURCE_DIR}/tests/unit/doc" "${CMAKE_SOURCE_DIR}/tests/unit/core_jobs")
target_compile_definitions(ray_cam_contour2d_tests PRIVATE RAY_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus/files"
    RAY_TOOLPATH_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden/toolpaths"
    RAY_POST_TEST_DIR="${CMAKE_SOURCE_DIR}/tests/unit/post" RAY_SCHEMA_DIR="${CMAKE_SOURCE_DIR}/schemas"
    RAY_DOC_KERNEL_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/unit/kernel/data")
ray_add_test(ray_cam_contour2d_tests)
# Operation templates (task T-057): the core (src/cam/templates.h) against hand-made holes and the shipped library, the
# shipped starter templates, and the cam.template.* actions on a part built like benchmark part 1 in a headless app.
add_executable(ray_cam_templates_tests
    templates_api_test.cpp
    templates_test.cpp
)
target_link_libraries(ray_cam_templates_tests PRIVATE ray_api ray_store ray_doc ray_cam ray_sim ray_post ray_naming
    ray_kernel ray_lib ray_core Catch2::Catch2WithMain)
target_include_directories(ray_cam_templates_tests PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/doc"
    "${CMAKE_SOURCE_DIR}/tests/unit/doc" "${CMAKE_SOURCE_DIR}/tests/unit/core_jobs")
target_compile_definitions(ray_cam_templates_tests PRIVATE RAY_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    RAY_DOC_KERNEL_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/unit/kernel/data")
ray_add_test(ray_cam_templates_tests)
# Cycle time (task T-059): the hand-calculated cases, the bits on every platform, the 1,000,000-move budget ([wallclock],
# split out by tests/tiers.cmake) and the comparison with times measured on real machines (tests/data/cycletime).
add_executable(ray_cam_cycletime_tests cycletime_test.cpp)
target_link_libraries(ray_cam_cycletime_tests PRIVATE ray_cam ray_lib ray_core Catch2::Catch2WithMain)
target_compile_definitions(ray_cam_cycletime_tests PRIVATE
    RAY_CYCLETIME_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data/cycletime")
set_property(TARGET ray_cam_cycletime_tests PROPERTY FOLDER "tests")
add_test(NAME ray_cam_cycletime_tests COMMAND ray_cam_cycletime_tests "~[wallclock]")
ray_test_runtime_path(ray_cam_cycletime_tests)
# The toolpath invariant checker and the property tests of the operation types (task T-058).
add_subdirectory(invariants)
# The shared 2D geometry core of the milling operations (task T-050): conversions, measures, nesting, offsets and
# booleans with exact arcs, arc fitting (property tests, 10,000 seeded cases each), boundaries from kernel-built parts,
# the integer goldens (tests/golden/geom2d) and, tagged [wallclock], the 500-loop timing and the cancel latency.
# src/ for the private header (cam/geom2d.h), tests/unit/kernel for the profile builders.
add_executable(ray_cam_geom2d_tests
    geom2d_basic_test.cpp
    geom2d_fit_test.cpp
    geom2d_golden_test.cpp
    geom2d_model_test.cpp
    geom2d_offset_test.cpp
    geom2d_parts.h
    geom2d_perf_test.cpp
    geom2d_test_util.h
)
target_link_libraries(ray_cam_geom2d_tests PRIVATE ray_cam ray_kernel ray_lib ray_naming ray_core Catch2::Catch2WithMain)
target_include_directories(ray_cam_geom2d_tests PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/tests/unit/kernel")
target_compile_definitions(ray_cam_geom2d_tests PRIVATE RAY_GEOM2D_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden/geom2d")
set_property(TARGET ray_cam_geom2d_tests PROPERTY FOLDER "tests")
add_test(NAME ray_cam_geom2d_tests COMMAND ray_cam_geom2d_tests "~[wallclock]")
ray_test_runtime_path(ray_cam_geom2d_tests)
# Linking (task T-051): each lead, ramp and helix with its exact geometry and the entry fall-back order, the two
# acceptance properties on seeded random cases (an independent height field, and the invariant checker of
# tests/unit/cam/invariants on part of them), the order against a brute-force best order on small cases and on passes
# shaped like the benchmark parts, the integer goldens (tests/golden/link) and, tagged [wallclock], the ordering time
# and the cancel latency. src/ for the private headers (cam/link.h, cam/geom2d.h).
add_executable(ray_cam_link_tests
    link_golden_test.cpp
    link_order_test.cpp
    link_parts.h
    link_perf_test.cpp
    link_property_test.cpp
    link_test_util.h
    link_unit_test.cpp
)
target_link_libraries(ray_cam_link_tests PRIVATE ray_cam_invariants ray_cam ray_kernel ray_lib ray_naming ray_core
    Catch2::Catch2WithMain)
target_include_directories(ray_cam_link_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(ray_cam_link_tests PRIVATE RAY_LINK_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden/link")
set_property(TARGET ray_cam_link_tests PROPERTY FOLDER "tests")
add_test(NAME ray_cam_link_tests COMMAND ray_cam_link_tests "~[wallclock]")
ray_test_runtime_path(ray_cam_link_tests)
# The 2D pocket operation (task T-054): depths, rings, offsets, finishing and entries against hand-computed values on
# kernel-built parts, each refusal, the coverage check, the goldens (tests/golden/toolpaths/pocket2d_*, with a corpus
# part), the chain through the backplot and the post host, and end to end through the document and the engine. Every
# toolpath goes through the independent invariant checker (invariants/) with coverage claims on the floors.
add_executable(ray_cam_pocket2d_tests
    pocket2d_document_test.cpp
    pocket2d_golden_test.cpp
    pocket2d_test.cpp
    pocket2d_test_util.h
    toolpath_dump.h
)
target_link_libraries(ray_cam_pocket2d_tests PRIVATE ray_cam_invariants ray_api ray_store ray_doc ray_cam ray_sim ray_post
    ray_naming ray_kernel ray_lib ray_core Catch2::Catch2WithMain)
target_include_directories(ray_cam_pocket2d_tests PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/doc"
    "${CMAKE_SOURCE_DIR}/tests/unit/doc" "${CMAKE_SOURCE_DIR}/tests/unit/core_jobs")
target_compile_definitions(ray_cam_pocket2d_tests PRIVATE RAY_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus/files"
    RAY_TOOLPATH_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden/toolpaths"
    RAY_POST_TEST_DIR="${CMAKE_SOURCE_DIR}/tests/unit/post" RAY_SCHEMA_DIR="${CMAKE_SOURCE_DIR}/schemas"
    RAY_DOC_KERNEL_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/unit/kernel/data")
set_property(TARGET ray_cam_pocket2d_tests PROPERTY FOLDER "tests")
add_test(NAME ray_cam_pocket2d_tests COMMAND ray_cam_pocket2d_tests "~[wallclock]")
ray_test_runtime_path(ray_cam_pocket2d_tests)
