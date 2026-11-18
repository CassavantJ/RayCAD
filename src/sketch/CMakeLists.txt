# sketch: Sketch entities, constraints, solver, DXF. Allowed dependencies: PLAN.md section 1.1.
add_library(ray_sketch STATIC sketch_module.cpp)
ray_module(ray_sketch DETERMINISTIC)
target_link_libraries(ray_sketch PRIVATE ray_core ray_kernel)
