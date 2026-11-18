# Queued calls from worker threads to the UI thread through Qt's event queue: QtUiDispatcher (scene_jobs.h) and
# invokeQueued (queued_call.h). Plain Catch2 on a QCoreApplication, no window.
find_package(Qt6 6.12 REQUIRED COMPONENTS Core)

add_executable(ray_ui_dispatch_tests dispatch_test.cpp)
# ray_api: <ray/api.h> (IUiDispatcher), which scene_jobs.h includes.
target_link_libraries(ray_ui_dispatch_tests PRIVATE ray_api Qt6::Core Catch2::Catch2)
# The private headers of src/ui (scene_jobs.h, queued_call.h).
target_include_directories(ray_ui_dispatch_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/ui")
set_property(TARGET ray_ui_dispatch_tests PROPERTY FOLDER "tests")

add_test(NAME ray_ui_dispatch_tests COMMAND ray_ui_dispatch_tests)
ray_test_runtime_path(ray_ui_dispatch_tests QT)

# Under ThreadSanitizer, the hidden case that hands a closure over with a bare invokeMethod must be reported: proof
# that the tests above would see a queued hand-off TSan cannot follow.
if(RAY_SANITIZE MATCHES "thread")
    add_test(NAME ray_ui_dispatch_tsan_selftest COMMAND ray_ui_dispatch_tests "[.tsan-selftest]")
    set_tests_properties(ray_ui_dispatch_tsan_selftest PROPERTIES PASS_REGULAR_EXPRESSION
                                                                  "WARNING: ThreadSanitizer: data race")
endif()
