# Sketch model and solver tests (task T-090).
add_executable(ray_sketch_tests
    sketch_test_util.h
    drag_runner.h
    sketch_det_hash.h
    solver_constraints_test.cpp
    solver_diagnosis_test.cpp
    solver_drag_test.cpp
    solver_determinism_test.cpp
    solver_cancel_test.cpp
    solver_scale_test.cpp
    drag_s11_test.cpp
    property_test.cpp
    loops_test.cpp
    infer_test.cpp
    dxf_corpus_expect.h
    dxf_test.cpp)
# ray_kernel: the loop tests extrude closedLoops output through the kernel (sketch may depend on kernel, PLAN.md 1.1).
target_link_libraries(ray_sketch_tests PRIVATE ray_sketch ray_kernel ray_core Catch2::Catch2WithMain)
# The private headers of src/sketch (model.h, solver_internal.h, loops.h, dxf.h: test hooks). None includes PlaneGCS,
# Eigen or OCCT.
target_include_directories(ray_sketch_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/sketch")
# The DXF corpus (tests/corpus, see its README); dxf_corpus_expect.h names files below it.
target_compile_definitions(ray_sketch_tests PRIVATE RAY_CORPUS_ROOT="${CMAKE_SOURCE_DIR}/tests/corpus")
set_property(TARGET ray_sketch_tests PROPERTY FOLDER "tests")

# The determinism probe: one process per core class (solver_determinism_test.cpp starts it).
add_executable(ray_sketch_det_probe sketch_det_probe.cpp)
target_link_libraries(ray_sketch_det_probe PRIVATE ray_sketch ray_core Catch2::Catch2)
target_include_directories(ray_sketch_det_probe PRIVATE "${CMAKE_SOURCE_DIR}/src/sketch")
set_property(TARGET ray_sketch_det_probe PROPERTY FOLDER "tests")
target_compile_definitions(ray_sketch_tests PRIVATE RAY_SKETCH_DET_PROBE="$<TARGET_FILE:ray_sketch_det_probe>")
add_dependencies(ray_sketch_tests ray_sketch_det_probe)

# Fast tests, then the long ones each in a process and CTest entry of its own with a long timeout.
add_test(NAME ray_sketch_tests COMMAND ray_sketch_tests "~[property]~[agreement]~[bench]")
add_test(NAME ray_sketch_property COMMAND ray_sketch_tests "[property]")
add_test(NAME ray_sketch_drag_agreement COMMAND ray_sketch_tests "[agreement]")
add_test(NAME ray_sketch_drag_bench COMMAND ray_sketch_tests "[bench]")
foreach(_t ray_sketch_tests ray_sketch_property ray_sketch_drag_agreement ray_sketch_drag_bench)
    ray_test_runtime_path(${_t})
endforeach()
set_tests_properties(ray_sketch_tests PROPERTIES TIMEOUT 900)
set_tests_properties(ray_sketch_property ray_sketch_drag_agreement ray_sketch_drag_bench PROPERTIES TIMEOUT 3600)
