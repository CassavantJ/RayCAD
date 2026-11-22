# Canned cycles across modules (the canned-cycle settlement of cam.h): the backplot (src/sim) and the post host's
# expansion (src/post) must move the same way. sim and post may not depend on each other (PLAN.md 1.1; api is the
# first module that links both), so this test links the two itself and lives apart from either module's tests.
add_executable(ray_cycle_agreement_tests cycle_agreement_test.cpp)
target_link_libraries(ray_cycle_agreement_tests PRIVATE ray_sim ray_post ray_core Catch2::Catch2WithMain)
# engine_motion.h is private to src/post (it includes no QuickJS header).
target_include_directories(ray_cycle_agreement_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/post")
ray_add_test(ray_cycle_agreement_tests)
