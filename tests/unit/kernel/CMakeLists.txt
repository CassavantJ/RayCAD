# Kernel tests. occt_smoke_test.cpp calls into every toolkit ray_kernel links; on Windows it also proves that
# the toolkits are delay-loaded and that the set of DLLs loaded equals the list in src/kernel/CMakeLists.txt.
add_executable(ray_kernel_tests
    occt_smoke_test.cpp
    shape_test.cpp
    query_test.cpp
    mesh_test.cpp
    io_test.cpp
    sweep_test.cpp
    boolean_test.cpp
    blend_test.cpp
    blend_determinism_test.cpp
    immutability_test.cpp
    slice_test.cpp # sliceAt, silhouette, faceLoops, edgeCurves, minInternalCornerRadius (task T-050)
    directedit_test.cpp directedit_corpus_test.cpp) # deleteFaces, offsetFaces, resizeHole, unifySameDomain (T-080)
target_link_libraries(ray_kernel_tests PRIVATE ray_kernel Catch2::Catch2WithMain)
# occt_smoke.h, shape_testing.h, mesh_testing.h and io_internal.h are private to src/kernel; no OCCT header.
target_include_directories(ray_kernel_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/kernel")
target_compile_definitions(ray_kernel_tests PRIVATE RAY_EXPECTED_OCCT_VERSION="8.0.1"
    RAY_KERNEL_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    RAY_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus/files"
    RAY_CORPUS_ROOT_DIR="${CMAKE_SOURCE_DIR}/tests/corpus")
set_property(TARGET ray_kernel_tests PROPERTY FOLDER "tests")

# The smoke test needs a process in which no OCCT DLL has been loaded yet, and Catch2 runs test cases in
# random order, so it runs in a process of its own ([occt] tag) and everything else in another.
add_test(NAME ray_kernel_occt_smoke COMMAND ray_kernel_tests "[occt]")
add_test(NAME ray_kernel_tests
    COMMAND ray_kernel_tests "~[occt]~[stress]~[wallclock]~[blend]~[directedit_corpus]~[directedit_nist]")
# The blend tests (with the blend determinism test) are about two thirds of the suite's time: a process of their own,
# so a parallel ctest runs them beside the rest.
add_test(NAME ray_kernel_blend COMMAND ray_kernel_tests "[blend]~[stress]")
# The 8-thread blend stress over the corpus (task T-095) in a process of its own, with its own timeout.
add_test(NAME ray_kernel_blend_stress COMMAND ray_kernel_tests "[stress]")
ray_test_runtime_path(ray_kernel_occt_smoke)
ray_test_runtime_path(ray_kernel_tests)
ray_test_runtime_path(ray_kernel_blend)
ray_test_runtime_path(ray_kernel_blend_stress)
# The direct-edit success rates on the S9 set (task T-080) in a process of their own: every case has its own time limit,
# and abandoned (timed-out) edits finish on kernel threads in the background.
add_test(NAME ray_kernel_directedit_corpus COMMAND ray_kernel_tests "[directedit_corpus]")
ray_test_runtime_path(ray_kernel_directedit_corpus)
# The same rates on the wider set of NIST test models (tests/corpus/directedit/set_nist.tsv), mostly fetch-only: skipped
# (exit code 4) when none of its parts has been fetched.
add_test(NAME ray_kernel_directedit_nist COMMAND ray_kernel_tests "[directedit_nist]")
ray_test_runtime_path(ray_kernel_directedit_nist)
# Import, meshing and concurrency tests are slow under ThreadSanitizer and on a loaded machine.
set_tests_properties(ray_kernel_tests ray_kernel_blend ray_kernel_blend_stress PROPERTIES TIMEOUT 900)
set_tests_properties(ray_kernel_directedit_corpus PROPERTIES TIMEOUT 1800)
set_tests_properties(ray_kernel_directedit_nist PROPERTIES TIMEOUT 1800 SKIP_RETURN_CODE 4)

# Imports one STEP or IGES file and prints what came out; tools/import_corpus.py runs it on every file of a
# corpus folder, one child process per file, to count passes, failures, crashes and hangs.
add_executable(ray_kernel_io_probe io_probe.cpp)
target_link_libraries(ray_kernel_io_probe PRIVATE ray_kernel ray_core)
target_include_directories(ray_kernel_io_probe PRIVATE "${CMAKE_SOURCE_DIR}/src/kernel")
set_property(TARGET ray_kernel_io_probe PROPERTY FOLDER "tests")
add_test(NAME ray_kernel_io_probe COMMAND ray_kernel_io_probe "${CMAKE_CURRENT_SOURCE_DIR}/data/assembly_mm.step")
ray_test_runtime_path(ray_kernel_io_probe)
