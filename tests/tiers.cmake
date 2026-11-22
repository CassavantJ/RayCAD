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
    ray_sketch_drag_agreement
    ray_render_backplot_cost) # about 200 s: mostly meshing its 500k-triangle baseline; guards src/render path drawing only

# Disabled in the Debug configuration of an MSVC build only (see MSVC DEBUG above).
set(RAY_MSVC_DEBUG_DISABLED_TESTS
    ray_naming_random
    ray_naming_session
    ray_naming_random_holdout
    ray_naming_random_holdout2
    ray_naming_random_holdout3
    ray_naming_random_holdout4
    ray_naming_random_holdout5
    ray_naming_random_holdout6)

# Quick tests that take over 20 minutes each in an MSVC Debug build (measured 25 to 28 minutes on 32 hardware threads,
# against about a minute in Release): the S4 scenarios, at the naming level and as document features. They also get
# the label debug-heavy, which the Windows Debug CI job leaves out (test preset windows-msvc-debug-ci) to fit its time
# limit; Release on Windows, GCC and Clang Debug, and AddressSanitizer run them. The tests run there as anywhere else
# when asked for.
set(RAY_DEBUG_HEAVY_TESTS
    ray_naming_s4
    ray_doc_s4
    ray_doc_s4_revolve
    ray_doc_s4_hole)

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
    ray_naming_s4=7             # 7.0
    ray_naming_random_smoke=5   # 4.8
    ray_doc_s4=6                # 6.0
    ray_doc_s4_revolve=7        # 6.8
    ray_doc_s4_hole=6           # 6.4
    ray_kernel_blend_stress=2)  # 2.0 (8 threads that mostly wait on each other)

# Suites with test cases tagged [wallclock], as "<test>=<executable target>". The suite's own command leaves those
# cases out ("~[wallclock]"); here a test <test>_wallclock runs just them ("[wallclock]"), alone, with the suite's
# environment and timeout. Catch2 fails a run whose filter matches no test case, so a split cannot go empty unnoticed.
# (Not [timing]: the kernel tests already have a hidden "[.timing]" benchmark, which "[timing]" would select.)
set(RAY_WALLCLOCK_SPLITS
    ray_kernel_tests=ray_kernel_tests
    ray_cam_holes_tests=ray_cam_holes_tests
    ray_sketch_tests=ray_sketch_tests
    ray_store_tests=ray_store_tests
    ray_sim_tests=ray_sim_tests
    ray_post_tests=ray_post_tests
    ray_ui_start_tests=ray_ui_start_tests
    ray_ui_viewport_tests=ray_ui_viewport_tests
    ray_ui_measure_tests=ray_ui_measure_tests
    ray_ui_sketch_tests=ray_ui_sketch_tests
    ray_ui_settings_tests=ray_ui_settings_tests
    ray_ui_model_tests=ray_ui_model_tests)

# Whole tests timed against the wall clock, besides the <test>_wallclock splits: they run alone.
set(RAY_SERIAL_TESTS
    ray_core_jobs_tests     # a cancelled job returns within 100 ms
    ray_sketch_drag_bench   # the median drag step under 16 ms
    ray_store_bench         # journal append median under 10 ms, loadHead of a 20 MB file under 200 ms
    ray_render_backplot_cost) # 2 M backplot segments under the 500k-triangle baseline frame time on WARP

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
    clipboard=ray_ui_measure_tests) # copies a value and the whole result, then reads the clipboard back

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
