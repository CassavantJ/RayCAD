# doc: Document snapshots, features, rebuild, commands, undo. Allowed dependencies: PLAN.md section 1.1.
add_library(ray_doc STATIC
    doc_module.cpp
    expr.cpp
    json.cpp
    json_number.cpp
    state.cpp
)
ray_module(ray_doc DETERMINISTIC)
target_link_libraries(ray_doc PRIVATE ray_core ray_kernel ray_naming ray_sketch ray_cam)
