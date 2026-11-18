# api: Action registry, app session, headless app. Allowed dependencies: PLAN.md section 1.1.
add_library(ray_api STATIC api_module.cpp)
ray_module(ray_api)
target_link_libraries(ray_api PRIVATE ray_core ray_kernel ray_naming ray_sketch ray_lib ray_doc ray_store ray_cam ray_sim ray_post)
