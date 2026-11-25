# Test tiers and parallel scheduling. Included at the end of tests/CMakeLists.txt, once every test exists.
#
# TIERS. Every test gets exactly one CTest label:
#   quick  everything that guards ordinary changes. `ctest --preset <config>-quick` runs these (README, "Which tests
#          to run": every merge).
#   slow   the long randomised naming blocks and the like. They guard src/naming, src/kernel, ports/ and the naming
#          oracle, and matter for little else; the full presets (`ctest --preset <config>`) run them with the rest.
# A test is quick unless it is listed in RAY_SLOW_TESTS. Naming coverage in the quick tier: the S4 scenarios
# (ray_naming_s4), the acceptance and regression replays (ray_naming_oracle_tests) and the smoke block
# (ray_naming_random_smoke: the first sequences of every part of the main randomised block, judged by the same gates).
#
# MSVC DEBUG. The randomised naming blocks run about 40 times slower in an MSVC Debug build than in Release
# (unoptimised OpenCASCADE and MSVC's checked iterators), longer than a CI job may take. They are DISABLED there,
# in an MSVC build through the generator expression $<CONFIG:Debug>, so only its Debug configuration skips them; ctest lists them as
# "Not Run (Disabled)" instead of dropping them silently. GCC and Clang Debug builds and every Release build run them.
#
# SCHEDULING. The presets run ctest in parallel (execution.jobs). For that to be reliable:
#   PROCESSORS     tests that run several threads of their own say how many processor slots they take, so ctest does
#                  not start more work than the machine has; the value is about their CPU time over wall time.
#   RUN_SERIAL     tests whose pass criterion is a wall-clock budget (a cancel latency, a UI-thread gap, a frame time,
#                  a benchmark) run alone. They are split out of their suites by the Catch2 tag [wallclock], so only
#                  the timed test cases wait for an idle machine.
#   RESOURCE_LOCK  tests that need a resource of the whole desktop session hold a lock on it: "desktop" (the windows
#                  on the screen, the active window and the pointer) and "clipboard" (they set the system clipboard and read it back).
#                  The tests with the desktop lock, and the [wallclock] splits of their suites, also get the label
#                  desktop: `ctest -L desktop` runs every test that shows a window, `-LE desktop` none of them.
#                  RAY_TEST_SCREEN puts their windows on a screen other than the primary one (README).
# Every name listed below must exist: a renamed test fails the configure instead of quietly leaving its tier.

# The randomised naming blocks and the other suites over about 90 s in Release that routine changes do not need.
set(RAY_SLOW_TESTS
    ray_naming_random
    ray_naming_session
    ray_naming_random_holdout
    ray_naming_random_holdout2
    ray_naming_random_holdout3
    ray_naming_random_holdout4
    ray_naming_random_holdout5
    ray_naming_random_holdout6
    ray_naming_random_directedit # T-081b: 1,000 sequences with direct edits
    ray_sketch_drag_agreement
    ray_cam_invariants_slow   # the property tests of every operation type on their large seed sets (T-058)
    ray_cam_adaptive_slow     # the adaptive engine's acceptance on the S5 pockets, 32-run determinism (T-060)
    ray_bench_adaptive_review # docs/reports/adaptive.md rebuilt and compared (T-062)
    ray_render_backplot_cost  # about 200 s: mostly meshing its 500k-triangle baseline; guards src/render path drawing only
    ray_render_stock_cost     # T-073: a 0.05 mm stock under a 2 M-triangle budget; measurements for src/render only
    ray_ui_cam_slow_seek_tests  # the Simulate panel's slow-seek line from a real High run (2.5 GB, about 50 s)
    ray_kernel_directedit_corpus) # T-080: the direct-edit rates on the S9 set: 70 s alone, 112 s in a parallel ctest

# Disabled in the Debug configuration of an MSVC build only (see MSVC DEBUG above).
set(RAY_MSVC_DEBUG_DISABLED_TESTS
    ray_naming_random
    ray_naming_session
    ray_naming_random_holdout
    ray_naming_random_holdout2
    ray_naming_random_holdout3
    ray_naming_random_holdout4
    ray_naming_random_holdout5
    ray_naming_random_holdout6
    ray_naming_random_directedit
    ray_kernel_directedit_corpus) # T-080: 1,337 edits on unoptimised OpenCASCADE, many past their time limit

# Quick tests that take over 20 minutes each in an MSVC Debug build (measured 25 to 28 minutes on 32 hardware threads,
# against about a minute in Release): the S4 scenarios, at the naming level and as document features (and, below, the
# Release-scale benchmarks and the geometry core's 10,000-case sweeps, minutes in Debug). They also get
# the label debug-heavy, which the Debug CI jobs leave out (test presets <preset>-debug-ci) to fit their time limit:
# on the 4-processor runners even GCC Debug took almost three hours with them (most of it in the S4 suites, which use
# more processors than the runner has); every Release job and AddressSanitizer run them. The tests run there as
# anywhere else when asked for.
set(RAY_DEBUG_HEAVY_TESTS
    ray_naming_s4
    ray_doc_s4
    ray_doc_s4_revolve
    ray_doc_s4_hole
    # Release-scale benchmarks with absolute limits, which an MSVC Debug build cannot meet however idle the machine:
    # 3000 holes with references in under 30 s (41 s in Debug), and a 1,000,000-move post, which runs into the post
    # sandbox's own time limit in Debug.
    ray_cam_holes_tests_wallclock
    ray_post_tests_wallclock
    # T-060: the adaptive engine's PLAN.md section 9 run times (islands pocket in 3 s; about 2.8 s in Release).
    ray_cam_adaptive_tests_wallclock
    # T-061: the adaptive2d operation on the same pockets, and what it adds to the engine.
    ray_cam_adaptive2d_tests_wallclock
    # The geometry core's three 10,000-case arc-fitting sweeps (split from ray_cam_geom2d_tests by the tag [sweeps]):
    # most of that suite's 5 to 9 minutes in MSVC Debug, seconds in Release. The rest of the suite stays in Debug.
    ray_cam_geom2d_sweeps
    # T-075: BP1 generated nine times (three tier presets, 1, 4 and 16 workers): 35 to 52 s in Release, 14.5 minutes
    # in MSVC Debug (measured, CPU time over wall time 3.8).
    ray_cam_carried_tiers
    # T-070: the stock model's PLAN.md section 9 benchmark, 112k moves at 0.2, 0.1 and 0.05 mm (about a minute in
    # Release, far longer in MSVC Debug, where only its memory limits are checked).
    ray_sim_tests_wallclock
    # T-072: the simulator's run and seek on the same workload at the three cells (about 45 s in Release; a thinned
    # workload in Debug, which checks no time).
    ray_sim_simulator_bench
    # On the 4-processor Windows CI runner in MSVC Debug (runs 45129357124 and 45454955286), against a minute or two
    # in Release: the linking suite's seeded property and ordering cases (17 minutes, and once past its 20-minute
    # timeout; 12 minutes here on 32 hardware threads under load), the adaptive2d operation's suite (past its
    # 20-minute timeout) and adaptive2d's property tests on their quick seeds (24 minutes).
    ray_cam_link_tests
    ray_cam_adaptive2d_tests
    ray_cam_invariants_adaptive2d
    # The 2D pocket suite: past its 20-minute timeout in MSVC Debug on the runner (run 45528384530); 4.4 minutes in GCC
    # Debug there.
    ray_cam_pocket2d_tests)

# Slow tests that the AddressSanitizer CI job (sanitizers.yml) leaves out: under ASan they cannot finish within their
# timeout on the 4-processor runner. They get the label sanitizer-heavy; every Release job runs them. The property
# tests of every operation type on their large seed sets passed their 2-hour timeout under ASan even on 32 hardware
# threads (30 minutes in Release at most). The adaptive engine's slow acceptance (S5 pockets, 32-run determinism) took
# 45 minutes single-threaded under ASan on the runner (run 45528384533; about 5 minutes in a GCC Release build here).
set(RAY_SANITIZER_HEAVY_TESTS
    ray_cam_invariants_slow
    ray_cam_adaptive_slow
    ray_kernel_directedit_corpus) # T-080: 1,337 Defeaturing and boolean edits, each with a 20 s limit

# Tests whose pass or fail is a wall-clock budget, set for a developer's machine (PLAN.md section 9 rows, UI-thread
# gaps, benchmarks): every <test>_wallclock split below (RAY_WALLCLOCK_SPLITS) and these. They get the label bench, which
# the CI jobs leave out: the shared 4-processor runners are slower than the budgets assume and their speed varies.
# Local runs (tools\verify.cmd quick and full, the -quick and full presets) run them as before; the label changes no
# tier and no budget.
set(RAY_BENCH_TESTS
    ray_sketch_drag_bench    # the median drag step under 16 ms
    ray_store_bench          # journal append median under 10 ms, loadHead of a 20 MB file under 200 ms
    ray_sim_simulator_bench # T-072: simulation run and seek times per tier (section 9)
    ray_ui_cam_slow_seek_tests) # the slow-seek line at High needs the run's seek estimate over 100 ms (no time limit)

# Processor slots of the tests that run threads of their own: "<test>=<slots>", about their CPU time over wall time
# when run alone in Release (measured on 32 hardware threads). The naming blocks run up to 8 workers
# (RAYCAD_NAMING_WORKERS); the document S4 suites rebuild on the job system. Every other test measured at about one
# processor or less (the kernel, store, render and UI suites included). ctest caps a value above its job count.
set(RAY_TEST_PROCESSORS
    ray_naming_random=8
    ray_naming_session=8
    ray_naming_random_holdout=8
    ray_naming_random_holdout2=8
    ray_naming_random_holdout3=8
    ray_naming_random_holdout4=8
    ray_naming_random_holdout5=8
    ray_naming_random_holdout6=8
    ray_naming_random_directedit=8
    ray_naming_s4=7             # 7.0
    ray_naming_random_smoke=5   # 4.8
    ray_doc_s4=6                # 6.0
    ray_doc_s4_revolve=7        # 6.8
    ray_doc_s4_hole=6           # 6.4
    ray_kernel_blend_stress=2   # 2.0 (8 threads that mostly wait on each other)
    ray_cam_carried_tiers=4     # 3.8 (T-075: BP1 nine times over, on 1, 4 and 16 workers)
    ray_kernel_directedit_corpus=8) # T-080: 8 workers over the S9 set

# Suites with test cases tagged [wallclock], as "<test>=<executable target>". The suite's own command leaves those
# cases out ("~[wallclock]"); here a test <test>_wallclock runs just them ("[wallclock]"), alone, with the suite's
# environment and timeout. Catch2 fails a run whose filter matches no test case, so a split cannot go empty unnoticed.
# (Not [timing]: the kernel tests already have a hidden "[.timing]" benchmark, which "[timing]" would select.)
set(RAY_WALLCLOCK_SPLITS
    ray_kernel_tests=ray_kernel_tests
    ray_cam_holes_tests=ray_cam_holes_tests
    ray_cam_cycletime_tests=ray_cam_cycletime_tests
    ray_cam_geom2d_tests=ray_cam_geom2d_tests
    ray_cam_link_tests=ray_cam_link_tests
    ray_cam_adaptive_tests=ray_cam_adaptive_tests
    ray_cam_adaptive2d_tests=ray_cam_adaptive2d_tests # T-061
    ray_sketch_tests=ray_sketch_tests
    ray_store_tests=ray_store_tests
    ray_sim_tests=ray_sim_tests
    ray_post_tests=ray_post_tests
    ray_ui_start_tests=ray_ui_start_tests
    ray_ui_viewport_tests=ray_ui_viewport_tests
    ray_ui_measure_tests=ray_ui_measure_tests
    ray_ui_sketch_tests=ray_ui_sketch_tests
    ray_ui_settings_tests=ray_ui_settings_tests
    ray_ui_model_tests=ray_ui_model_tests
    ray_ui_cam_tests=ray_ui_cam_tests
    ray_ui_cam_operation_tests=ray_ui_cam_operation_tests
    ray_ui_cam_tools_tests=ray_ui_cam_tools_tests
    ray_ui_cam_post_tests=ray_ui_cam_post_tests
    ray_ui_cam_sim_tests=ray_ui_cam_sim_tests) # T-074

# Whole tests timed against the wall clock, besides the <test>_wallclock splits: they run alone.
set(RAY_SERIAL_TESTS
    ray_core_jobs_tests     # a cancelled job returns within 100 ms
    ray_sketch_drag_bench   # the median drag step under 16 ms
    ray_store_bench         # journal append median under 10 ms, loadHead of a 20 MB file under 200 ms
    ray_render_backplot_cost  # 2 M backplot segments under the 500k-triangle baseline frame time on WARP
    ray_render_stock_cost     # T-073: frame time of a 2 M-triangle stock against the same triangles as one mesh
    ray_sim_simulator_bench)  # T-072: simulation run and seek times per tier (section 9)

# "<lock>=<test>", one list entry per test and lock.
#   desktop    every test that shows a window. The windows of one desktop share the active window (only it gets keys
#              and the active focus), the real pointer and the screen: a test window that opens or closes takes the
#              focus from another one, moves the hover under the pointer and makes the windows it covers draw again.
#              Seen with two at once: a palette search field without the focus, an Esc that reached no dialog, a typed
#              radius that went nowhere, and frames drawn by an idle sketch window. (The [wallclock] splits run alone
#              anyway; the render tests draw offscreen and the dispatch tests show nothing.)
#   clipboard  tests that set the system clipboard and read it back.
set(RAY_TEST_LOCKS
    desktop=raycad_smoke
    desktop=ray_ui_controls_tests
    desktop=ray_ui_gallery_scale_1
    desktop=ray_ui_gallery_scale_1_5
    desktop=ray_ui_gallery_scale_2
    desktop=ray_ui_viewport_tests
    desktop=ray_ui_start_tests
    desktop=ray_ui_palette_tests
    desktop=ray_ui_settings_tests
    desktop=ray_ui_glass_tests_native
    desktop=ray_ui_glass_tests_1_5
    desktop=ray_ui_measure_tests
    desktop=ray_ui_section_tests
    desktop=ray_ui_sketch_tests
    desktop=ray_ui_model_tests
    desktop=ray_ui_cam_tests
    desktop=ray_ui_cam_operation_tests
    desktop=ray_ui_cam_operation_scale_1
    desktop=ray_ui_cam_operation_scale_1_5
    desktop=ray_ui_cam_operation_scale_2
    desktop=ray_ui_cam_tools_tests
    desktop=ray_ui_cam_post_tests
    desktop=ray_ui_cam_flow_tests
    desktop=ray_ui_cam_flow_m3
    desktop=ray_ui_cam_templates_tests
    desktop=ray_ui_cam_flow_scale_1
    desktop=ray_ui_cam_flow_scale_1_5
    desktop=ray_ui_cam_flow_scale_2
    desktop=ray_ui_cam_sheet_tests # T-076
    desktop=ray_ui_cam_fixtures_tests # T-077
    desktop=ray_ui_cam_sim_tests # T-074
    clipboard=ray_ui_measure_tests  # copies a value and the whole result, then reads the clipboard back
    clipboard=ray_ui_cam_post_tests) # copies G-code lines and the whole program, then reads the clipboard back

# ---------- implementation ----------
# Remembers the directory of every test (set_property(TEST ... DIRECTORY ...) needs it).
function(ray_index_tests dir)
    get_property(_tests DIRECTORY "${dir}" PROPERTY TESTS)
    foreach(_test IN LISTS _tests)
        set_property(GLOBAL APPEND PROPERTY RAY_ALL_TESTS "${_test}")
        set_property(GLOBAL PROPERTY "RAY_TEST_DIR_${_test}" "${dir}")
    endforeach()
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_subdir IN LISTS _subdirs)
        ray_index_tests("${_subdir}")
    endforeach()
endfunction()
ray_index_tests("${CMAKE_CURRENT_SOURCE_DIR}")

foreach(_entry IN LISTS RAY_WALLCLOCK_SPLITS)
    string(REPLACE "=" ";" _pair "${_entry}")
    list(GET _pair 0 _suite)
    list(GET _pair 1 _target)
    get_property(_dir GLOBAL PROPERTY "RAY_TEST_DIR_${_suite}")
    if(NOT _dir)
        message(FATAL_ERROR "tests/tiers.cmake splits the test '${_suite}', which does not exist")
    endif()
    add_test(NAME ${_suite}_wallclock COMMAND ${_target} "[wallclock]")
    foreach(_prop ENVIRONMENT ENVIRONMENT_MODIFICATION TIMEOUT SKIP_RETURN_CODE)
        get_property(_set TEST ${_suite} DIRECTORY "${_dir}" PROPERTY ${_prop} SET)
        if(_set)
            get_property(_value TEST ${_suite} DIRECTORY "${_dir}" PROPERTY ${_prop})
            set_property(TEST ${_suite}_wallclock PROPERTY ${_prop} "${_value}")
        endif()
    endforeach()
    set_property(GLOBAL APPEND PROPERTY RAY_ALL_TESTS "${_suite}_wallclock")
    set_property(GLOBAL PROPERTY "RAY_TEST_DIR_${_suite}_wallclock" "${CMAKE_CURRENT_SOURCE_DIR}")
    list(APPEND RAY_SERIAL_TESTS ${_suite}_wallclock)
    list(APPEND RAY_BENCH_TESTS ${_suite}_wallclock)
endforeach()
get_property(RAY_ALL_TESTS GLOBAL PROPERTY RAY_ALL_TESTS)

# ray_tier_set(<test> <APPEND|SET> <property> <value>...): sets a property of a test defined in any directory.
function(ray_tier_set test mode property)
    get_property(_dir GLOBAL PROPERTY "RAY_TEST_DIR_${test}")
    if(NOT _dir)
        message(FATAL_ERROR "tests/tiers.cmake names the test '${test}', which does not exist")
    endif()
    if(mode STREQUAL "APPEND")
        set_property(TEST "${test}" DIRECTORY "${_dir}" APPEND PROPERTY ${property} ${ARGN})
    else()
        set_property(TEST "${test}" DIRECTORY "${_dir}" PROPERTY ${property} ${ARGN})
    endif()
endfunction()

foreach(_test IN LISTS RAY_SLOW_TESTS)
    if(NOT _test IN_LIST RAY_ALL_TESTS)
        message(FATAL_ERROR "tests/tiers.cmake names the slow test '${_test}', which does not exist")
    endif()
endforeach()
foreach(_test IN LISTS RAY_ALL_TESTS)
    if(_test IN_LIST RAY_SLOW_TESTS)
        ray_tier_set(${_test} APPEND LABELS slow)
    else()
        ray_tier_set(${_test} APPEND LABELS quick)
    endif()
endforeach()
foreach(_test IN LISTS RAY_MSVC_DEBUG_DISABLED_TESTS)
    if(NOT _test IN_LIST RAY_SLOW_TESTS)
        message(FATAL_ERROR "tests/tiers.cmake: '${_test}' is disabled in MSVC Debug but is not a slow test")
    endif()
    if(MSVC)
        ray_tier_set(${_test} SET DISABLED "$<CONFIG:Debug>")
    endif()
endforeach()
foreach(_test IN LISTS RAY_DEBUG_HEAVY_TESTS)
    if(_test IN_LIST RAY_SLOW_TESTS)
        message(FATAL_ERROR "tests/tiers.cmake: '${_test}' is slow; debug-heavy is for quick tests")
    endif()
    ray_tier_set(${_test} APPEND LABELS debug-heavy)
endforeach()
foreach(_test IN LISTS RAY_SANITIZER_HEAVY_TESTS)
    ray_tier_set(${_test} APPEND LABELS sanitizer-heavy)
endforeach()
foreach(_test IN LISTS RAY_BENCH_TESTS)
    ray_tier_set(${_test} APPEND LABELS bench)
endforeach()
foreach(_entry IN LISTS RAY_TEST_PROCESSORS)
    string(REPLACE "=" ";" _pair "${_entry}")
    list(GET _pair 0 _test)
    list(GET _pair 1 _slots)
    ray_tier_set(${_test} SET PROCESSORS ${_slots})
endforeach()
foreach(_test IN LISTS RAY_SERIAL_TESTS)
    ray_tier_set(${_test} SET RUN_SERIAL TRUE)
endforeach()
foreach(_entry IN LISTS RAY_TEST_LOCKS)
    string(REPLACE "=" ";" _pair "${_entry}")
    list(GET _pair 0 _lock)
    list(GET _pair 1 _test)
    ray_tier_set(${_test} APPEND RESOURCE_LOCK ${_lock})
    # The desktop tests run one after another, about two and a half minutes in all. ctest starts tests in order of
    # COST (else their last run's time): a high cost puts each of them first whenever the lock is free, so the chain
    # runs from the start beside the long tests instead of after them.
    if(_lock STREQUAL "desktop")
        ray_tier_set(${_test} SET COST 10000)
        # The label desktop selects every test that shows a window (ctest -L desktop, -LE desktop): these, and the
        # [wallclock] split of such a suite (alone anyway, so without the lock; it shows windows too: the warm-start
        # cases start raycad.exe).
        ray_tier_set(${_test} APPEND LABELS desktop)
        if("${_test}_wallclock" IN_LIST RAY_ALL_TESTS)
            ray_tier_set(${_test}_wallclock APPEND LABELS desktop)
        endif()
    endif()
endforeach()
