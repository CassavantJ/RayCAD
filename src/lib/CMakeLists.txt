# lib: Tool, holder, material, machine, template and rule libraries. Allowed dependencies: PLAN.md section 1.1.
add_library(ray_lib STATIC lib_module.cpp)
ray_module(ray_lib DETERMINISTIC)
target_link_libraries(ray_lib PRIVATE ray_core)
