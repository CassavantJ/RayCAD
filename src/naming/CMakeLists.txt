# naming: Persistent names and reference resolution. Allowed dependencies: PLAN.md section 1.1.
add_library(ray_naming STATIC naming_module.cpp)
ray_module(ray_naming DETERMINISTIC)
target_link_libraries(ray_naming PRIVATE ray_core ray_kernel)
