# naming: Persistent names and reference resolution. Allowed dependencies: PLAN.md section 1.1.
add_library(ray_naming STATIC
    naming_module.cpp
    naming_internal.h element_map.cpp fingerprint.cpp
    import_naming.h name_imported.cpp
    resolver.h resolver.cpp
    propagate.h propagate.cpp propagate_resolve.cpp
    territory.h territory.cpp evidence.cpp
    rematch.h rematch.cpp # T-084: rematchImported
    topo_ref_json.cpp)
ray_module(ray_naming DETERMINISTIC)
target_link_libraries(ray_naming PRIVATE ray_core ray_kernel)
