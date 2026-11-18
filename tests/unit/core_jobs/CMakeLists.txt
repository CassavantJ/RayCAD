# Job system tests (T-003): cancellation, progress, JobHandle and IJobSystem on oneTBB.
add_executable(ray_core_jobs_tests cancel_progress_test.cpp jobs_test.cpp parallel_chunks_test.cpp)
target_link_libraries(ray_core_jobs_tests PRIVATE ray_core Catch2::Catch2WithMain)
ray_add_test(ray_core_jobs_tests)
