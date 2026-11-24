# sim: Backplot, stock model, safety checks, timeline. Allowed dependencies: PLAN.md section 1.1.
add_library(ray_sim STATIC
    sim_module.cpp
    backplot.h backplot.cpp
    toolshape.h toolshape.cpp # T-070
    tridexel.h tridexel_impl.h tridexel.cpp tridexel_cut.cpp tridexel_reset.cpp tridexel_mesh.cpp)
ray_module(ray_sim DETERMINISTIC)
target_link_libraries(ray_sim PRIVATE ray_core ray_kernel ray_cam ray_lib)
