# Tests (PLAN.md section 11). Everything here runs through CTest.
find_package(Catch2 3 CONFIG REQUIRED)
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

# Windows has no rpath: tests find the vcpkg DLLs (the delay-loaded OCCT toolkits) and, for the desktop app,
# the Qt DLLs through PATH. src/ui publishes the Qt folder, because only it may call find_package(Qt6).
function(ray_test_runtime_path test)
    if(WIN32)
        set(_dirs "${RAY_VCPKG_BIN_DIR}")
        if("QT" IN_LIST ARGN)
            get_property(_qt_bin GLOBAL PROPERTY RAY_QT_BIN_DIR)
            list(PREPEND _dirs "${_qt_bin}")
        endif()
        set(_mods)
        foreach(_dir IN LISTS _dirs)
            list(APPEND _mods "PATH=path_list_prepend:${_dir}")
        endforeach()
        set_tests_properties(${test} PROPERTIES ENVIRONMENT_MODIFICATION "${_mods}")
    endif()
endfunction()

# Registers a test executable as one CTest test.
function(ray_add_test target)
    add_test(NAME ${target} COMMAND ${target})
    set_property(TARGET ${target} PROPERTY FOLDER "tests")
    ray_test_runtime_path(${target})
endfunction()

add_subdirectory(headers)
add_subdirectory(unit/modules)
add_subdirectory(unit/core)
add_subdirectory(unit/kernel)
add_subdirectory(unit/doc)
add_subdirectory(unit/naming)
add_subdirectory(unit/sketch)
add_subdirectory(naming/oracle)
add_subdirectory(unit/store)
add_subdirectory(unit/lib)
add_subdirectory(unit/api)
add_subdirectory(unit/cam) # T-032
add_subdirectory(unit/render)
add_subdirectory(unit/core_jobs)
add_subdirectory(unit/sim)
add_subdirectory(unit/post) # T-039
add_subdirectory(unit/cycles) # canned cycles: backplot and post host agree
add_subdirectory(lint)
add_subdirectory(apps)
add_subdirectory(ui/screen) # RAY_TEST_SCREEN, for the window tests below
add_subdirectory(ui/controls)
add_subdirectory(ui/viewport)
add_subdirectory(ui/start)
add_subdirectory(ui/palette)
add_subdirectory(ui/settings)
add_subdirectory(ui/glass)
add_subdirectory(ui/measure)
add_subdirectory(ui/section)
add_subdirectory(ui/dispatch)
add_subdirectory(ui/sketch)
add_subdirectory(ui/model) # T-098
add_subdirectory(ui/cam) # T-044, T-045
# (Keep every add_subdirectory above this line: the sanitizer options below reach only the tests defined so far.)

# ---------- Graphics driver threads ----------
# Every test that draws with Direct3D 11 raises the idle-priority worker threads of the graphics driver to normal
# priority while it runs, so that they are not starved when every processor is busy (support/
# driver_threads_listener.cpp says why). Compiles to nothing off Windows.
foreach(_target IN ITEMS ray_render_tests ray_ui_controls_tests ray_ui_viewport_tests ray_ui_start_tests
                         ray_ui_palette_tests ray_ui_settings_tests ray_ui_glass_tests ray_ui_measure_tests
                         ray_ui_section_tests ray_ui_sketch_tests ray_ui_model_tests ray_ui_cam_tests
                         ray_ui_cam_operation_tests)
    target_sources(${_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/support/driver_threads_listener.cpp")
endforeach()

# ---------- The screen the windows go on ----------
# RAY_TEST_SCREEN (ui/screen/test_screen.h; README, "Which tests to run"): every test that shows a window puts its
# windows, and those of the raycad.exe it starts, on the screen the variable names instead of the primary one, and
# fails if one touches the primary screen. Unset (CI), nothing changes. The render tests draw offscreen (their own
# QRhi, no window) and raycad_smoke is placed by support/run_with_driver_priority.py.
foreach(_target IN ITEMS ray_ui_controls_tests ray_ui_viewport_tests ray_ui_start_tests ray_ui_palette_tests
                         ray_ui_settings_tests ray_ui_glass_tests ray_ui_measure_tests ray_ui_section_tests
                         ray_ui_sketch_tests ray_ui_model_tests ray_ui_cam_tests ray_ui_cam_operation_tests)
    target_sources(${_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ui/screen/test_screen_listener.cpp")
    target_link_libraries(${_target} PRIVATE ray_ui_test_screen)
endforeach()

# ---------- The rest of the desktop ----------
# The same tests ignore the system pointer over their windows and other programs taking the activation: they drive
# their windows with Qt's own event injection, and where the owner's mouse rests or which window another test opens
# must not change what they do (ui/foreign_input_listener.cpp says why). A test that needs the keyboard focus takes it
# with ray::test::focusWindow, which does not depend on the desktop granting it (ui/window_focus.h).
# The listener compiles to nothing off Windows.
find_package(Qt6 6.12 REQUIRED COMPONENTS GuiPrivate)
foreach(_target IN ITEMS ray_ui_controls_tests ray_ui_viewport_tests ray_ui_start_tests ray_ui_palette_tests
                         ray_ui_settings_tests ray_ui_glass_tests ray_ui_measure_tests ray_ui_section_tests
                         ray_ui_sketch_tests ray_ui_model_tests ray_ui_cam_tests ray_ui_cam_operation_tests)
    target_sources(${_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ui/foreign_input_listener.cpp"
                                      "${CMAKE_CURRENT_SOURCE_DIR}/ui/window_focus.cpp")
    target_link_libraries(${_target} PRIVATE Qt6::GuiPrivate) # window_focus.cpp: QWindowSystemInterface
endforeach()

# ---------- The UI-thread watchdog ----------
# The stall tests ([wallclock]) judge the time the UI thread itself was busy or blocked in its worst gap, not the time
# it was runnable and not run (ui/ui_watchdog.h).
foreach(_target IN ITEMS ray_ui_viewport_tests ray_ui_start_tests ray_ui_measure_tests ray_ui_sketch_tests
                         ray_ui_model_tests)
    target_sources(${_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ui/ui_watchdog.cpp")
endforeach()

# ---------- Sanitizer run options for every test ----------
# Adds `options` to the environment variable `var` (ASAN_OPTIONS, ...) of every test under `dir`. A test that sets
# the variable itself (a suppression file) keeps its options; these are added to them.
function(ray_sanitizer_test_options dir var options)
    get_property(_tests DIRECTORY "${dir}" PROPERTY TESTS)
    foreach(_test IN LISTS _tests)
        get_property(_env TEST "${_test}" DIRECTORY "${dir}" PROPERTY ENVIRONMENT)
        set(_merged)
        set(_found FALSE)
        foreach(_entry IN LISTS _env)
            if(_entry MATCHES "^${var}=")
                string(APPEND _entry ":${options}")
                set(_found TRUE)
            endif()
            list(APPEND _merged "${_entry}")
        endforeach()
        if(NOT _found)
            list(APPEND _merged "${var}=${options}")
        endif()
        set_property(TEST "${_test}" DIRECTORY "${dir}" PROPERTY ENVIRONMENT "${_merged}")
    endforeach()
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_subdir IN LISTS _subdirs)
        ray_sanitizer_test_options("${_subdir}" "${var}" "${options}")
    endforeach()
endfunction()

# AddressSanitizer and UBSan: the options .github/workflows/sanitizers.yml runs ctest with, so a local ctest
# (tools/linux-local.sh) checks the same. Leak detection is off there and here: Qt, Fontconfig and OpenCASCADE keep
# allocations LeakSanitizer reports (OpenCASCADE's IGES reader leaks entities of a file it fails to read).
if(RAY_SANITIZE MATCHES "address")
    ray_sanitizer_test_options("${CMAKE_CURRENT_SOURCE_DIR}" ASAN_OPTIONS
                               "detect_leaks=0:strict_string_checks=1:detect_stack_use_after_return=1")
endif()
if(RAY_SANITIZE MATCHES "undefined")
    ray_sanitizer_test_options("${CMAKE_CURRENT_SOURCE_DIR}" UBSAN_OPTIONS "print_stacktrace=1:halt_on_error=1")
endif()

# ThreadSanitizer. Only RayCAD's code and oneTBB are instrumented (the overlay triplet); Qt, OpenCASCADE, SQLite,
# Mesa and the other libraries are not, so TSan cannot see their synchronisation and reports races between their
# own threads that pass through the interceptors it puts in our executables (Qt's QQmlThread freeing what the main
# thread allocated, through the instrumented operator new and delete). ignore_noninstrumented_modules ignores
# intercepted calls made from code that is not instrumented, and nothing else: every memory access in RayCAD's code
# and in oneTBB is still checked, and the deliberate race of ray_core_jobs_tsan_selftest is still reported.
if(RAY_SANITIZE MATCHES "thread")
    ray_sanitizer_test_options("${CMAKE_CURRENT_SOURCE_DIR}" TSAN_OPTIONS "ignore_noninstrumented_modules=1")
    # The Qt Quick tests run the application, whose job system starts one oneTBB worker per hardware thread. Under
    # TSan every atomic operation takes a TSan mutex, and with 32 workers spinning in oneTBB's sleep/wake protocol on
    # a loaded machine the UI thread was starved inside such a mutex (ray_ui_start_tests hit its 900 s limit; it
    # takes about 10 s otherwise). These tests get 4 workers (RAYCAD_WORKER_THREADS, read by makeJobSystem). Nothing
    # is checked less.
    function(ray_tsan_ui_workers dir)
        if(dir MATCHES "/tests/ui/")
            get_property(_tests DIRECTORY "${dir}" PROPERTY TESTS)
            foreach(_test IN LISTS _tests)
                set_property(TEST "${_test}" DIRECTORY "${dir}" APPEND PROPERTY ENVIRONMENT "RAYCAD_WORKER_THREADS=4")
            endforeach()
        endif()
        get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
        foreach(_subdir IN LISTS _subdirs)
            ray_tsan_ui_workers("${_subdir}")
        endforeach()
    endfunction()
    ray_tsan_ui_workers("${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# ---------- Debug timeouts ----------
# A Debug build runs unoptimised OpenCASCADE, oneTBB and Qt (vcpkg's and Qt's debug libraries) and our own code at
# /Od: the corpus, blend and naming suites take four to eight times as long as in Release (ray_kernel_tests about
# 120 s in Release, past its 900 s limit in Debug). Every test's timeout (the CTest presets' 300 s when it sets none)
# is RAY_DEBUG_TIMEOUT_FACTOR times longer in the Debug configuration; Release and RelWithDebInfo keep theirs. A
# timeout is a guard against hangs, not a performance budget: the benchmarks check their budgets themselves.
set(RAY_DEBUG_TIMEOUT_FACTOR 4)
function(ray_debug_timeouts dir)
    get_property(_tests DIRECTORY "${dir}" PROPERTY TESTS)
    foreach(_test IN LISTS _tests)
        get_property(_timeout TEST "${_test}" DIRECTORY "${dir}" PROPERTY TIMEOUT)
        if(NOT _timeout)
            set(_timeout 300)
        endif()
        math(EXPR _debug "${_timeout} * ${RAY_DEBUG_TIMEOUT_FACTOR}")
        set_property(TEST "${_test}" DIRECTORY "${dir}" PROPERTY TIMEOUT
                     "$<IF:$<CONFIG:Debug>,${_debug},${_timeout}>")
    endforeach()
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_subdir IN LISTS _subdirs)
        ray_debug_timeouts("${_subdir}")
    endforeach()
endfunction()
ray_debug_timeouts("${CMAKE_CURRENT_SOURCE_DIR}")

include(tiers.cmake)
