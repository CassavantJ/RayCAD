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
add_subdirectory(unit/store)
add_subdirectory(unit/api)
add_subdirectory(unit/render)
add_subdirectory(unit/core_jobs)
add_subdirectory(lint)
add_subdirectory(apps)
add_subdirectory(ui/controls)
add_subdirectory(ui/viewport)
add_subdirectory(ui/start)
add_subdirectory(ui/palette)
add_subdirectory(ui/glass)
add_subdirectory(ui/section)
# (Keep every add_subdirectory above this line: the sanitizer options below reach only the tests defined so far.)

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
endif()
