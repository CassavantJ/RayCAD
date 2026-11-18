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
add_subdirectory(unit/kernel)
add_subdirectory(unit/core_jobs)
add_subdirectory(lint)
add_subdirectory(apps)
