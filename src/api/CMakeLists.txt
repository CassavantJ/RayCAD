# api: Action registry, app session, headless app. Allowed dependencies: PLAN.md section 1.1.
add_library(ray_api STATIC
    api_module.cpp
    include/ray_api/viewer.h
    include/ray_api/recent_files.h
    include/ray_api/settings.h
    settings.cpp
    include/ray_api/measure.h
    include/ray_api/sketch_edit.h
    sketch_edit.cpp
    include/ray_api/feature_edit.h
    feature_edit.cpp
    include/ray_api/cam_setup.h # the CAM workspace's setup preview and helpers (T-044)
    cam_setup.cpp
    include/ray_api/cam_operation.h # the operation panel's types, feeds, holes and backplot (T-045)
    cam_operation.cpp
    include/ray_api/cam_tools.h # the tool library manager's item formats and checked saves (T-046)
    cam_tools.cpp
    viewer.cpp
    measure.cpp
    measure_actions.h
    measure_actions.cpp
    app.cpp
    builtin_actions.cpp
    cam_wiring.cpp     # operation registry, CAM engine, library stack and post engine of the app
    describe.cpp
    files.cpp
    program_dir.cpp
    json.cpp
    recovery.cpp
    registry.cpp
    schema.cpp
    search.cpp
    ui_thread.cpp
)
ray_module(ray_api)
# ray_api/viewer.h: what the desktop viewer (src/ui) needs from kernel and naming, without Qt (task T-020).
target_include_directories(ray_api PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ray_api PRIVATE ray_core ray_kernel ray_naming ray_sketch ray_lib ray_doc ray_store ray_cam ray_sim ray_post)

# The built-in library (resources/library) beside the programs, where the app looks for it (api.h
# AppOptions::builtinLibraryDir): <bin>/resources/library in the build tree, as an installed Windows program has it.
# Every executable of the build (the apps and the tests) is in that folder. Copied again when a file changes.
file(GLOB_RECURSE RAY_BUILTIN_LIBRARY_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/resources/library/*")
get_property(_ray_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_ray_multi_config)
    set(_ray_bin "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>")
else()
    set(_ray_bin "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()
set(_ray_library_stamp "${CMAKE_CURRENT_BINARY_DIR}/builtin_library_$<CONFIG>.stamp")
add_custom_command(OUTPUT "${_ray_library_stamp}"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_ray_bin}/resources/library"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/resources/library" "${_ray_bin}/resources/library"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_ray_library_stamp}"
    DEPENDS ${RAY_BUILTIN_LIBRARY_FILES}
    COMMENT "Copying the built-in library beside the programs"
    VERBATIM)
add_custom_target(ray_builtin_library DEPENDS "${_ray_library_stamp}")
set_property(TARGET ray_builtin_library PROPERTY FOLDER "resources")
add_dependencies(ray_api ray_builtin_library)
