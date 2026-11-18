# doc: Document snapshots, features, rebuild, commands, undo. Allowed dependencies: PLAN.md section 1.1.
add_library(ray_doc STATIC
    commands/args.cpp
    commands/command_json.cpp
    commands/model_commands.cpp
    commands/registry.cpp
    doc_module.cpp
    document.cpp
    expr.cpp
    features/import_command.cpp
    features/import_feature.cpp
    features/import_props.cpp
    json.cpp
    json_number.cpp
    rebuild.cpp
    rebuild_objects.cpp
    rebuild_registry.cpp
    state.cpp
)
ray_module(ray_doc DETERMINISTIC)
target_link_libraries(ray_doc PRIVATE ray_core ray_kernel ray_naming ray_sketch ray_cam)
