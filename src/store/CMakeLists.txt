# store: Native file, objects, commits, recovery journal, migrations. Allowed dependencies: PLAN.md section 1.1.
add_library(ray_store STATIC store_module.cpp)
ray_module(ray_store DETERMINISTIC)
target_link_libraries(ray_store PRIVATE ray_core ray_doc)
