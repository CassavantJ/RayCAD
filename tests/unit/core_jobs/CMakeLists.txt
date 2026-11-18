# Job system tests (T-003): cancellation, progress, JobHandle and IJobSystem on oneTBB.
add_executable(ray_core_jobs_tests cancel_progress_test.cpp jobs_test.cpp parallel_chunks_test.cpp tsan_selftest.cpp)
target_link_libraries(ray_core_jobs_tests PRIVATE ray_core Catch2::Catch2WithMain)
ray_add_test(ray_core_jobs_tests)

# Under ThreadSanitizer, the hidden racy test must be reported: proof that races in job code are still seen.
if(RAY_SANITIZE MATCHES "thread")
    add_test(NAME ray_core_jobs_tsan_selftest COMMAND ray_core_jobs_tests "[.tsan-selftest]")
    set_tests_properties(ray_core_jobs_tsan_selftest PROPERTIES PASS_REGULAR_EXPRESSION
                                                                "WARNING: ThreadSanitizer: data race")
endif()
