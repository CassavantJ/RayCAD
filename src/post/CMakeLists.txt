# post: Post engine, G-code output, setup sheet. Allowed dependencies: PLAN.md section 1.1.
add_library(ray_post STATIC post_module.cpp)
ray_module(ray_post DETERMINISTIC)
target_link_libraries(ray_post PRIVATE ray_core ray_cam ray_lib)
