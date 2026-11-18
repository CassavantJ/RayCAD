# Links the whole headless stack and checks each module's link edges against PLAN.md section 1.1.
add_executable(ray_module_tests module_graph_test.cpp)
target_link_libraries(ray_module_tests PRIVATE ray_api Catch2::Catch2WithMain)
ray_add_test(ray_module_tests)
