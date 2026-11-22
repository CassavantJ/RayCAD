# cam: Setups, operations, toolpath IR, generators. Allowed dependencies: PLAN.md section 1.1.
add_library(ray_cam STATIC
    cam_module.cpp
    engine.cpp   # ICamEngine, makeCamEngine, resolveGeometryBasic (task T-032)
    feeds.cpp    # computeFeeds (task T-048)
    state.cpp    # OperationRegistry, operationInputHash (task T-032)
    state.h
)
ray_module(ray_cam DETERMINISTIC)
target_link_libraries(ray_cam PRIVATE ray_core ray_kernel ray_naming ray_lib)
