# api: Action registry, app session, headless app. Allowed dependencies: PLAN.md section 1.1.
add_library(ray_api STATIC
    api_module.cpp
    include/ray_api/viewer.h
    viewer.cpp)
ray_module(ray_api)
# ray_api/viewer.h: what the desktop viewer (src/ui) needs from kernel and naming, without Qt (task T-020).
target_include_directories(ray_api PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ray_api PRIVATE ray_core ray_kernel ray_naming ray_sketch ray_lib ray_doc ray_store ray_cam ray_sim ray_post)
