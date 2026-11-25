# sim: Backplot, stock model, safety checks, timeline. Allowed dependencies: PLAN.md section 1.1.
add_library(ray_sim STATIC
    sim_module.cpp
    backplot.h backplot.cpp
    toolshape.h toolshape.cpp # T-070
    tridexel.h tridexel_impl.h tridexel.cpp tridexel_cut.cpp tridexel_reset.cpp tridexel_mesh.cpp
    verify.h verify_impl.h verify_geom.h verify.cpp verify_geom.cpp verify_motion.cpp verify_cutstock.cpp # T-071
    verify_setup.cpp # fixtures against the part and the stock (after T-074)
    simulator.h simulator.cpp simulator_gouge.cpp) # T-072
ray_module(ray_sim DETERMINISTIC)
target_link_libraries(ray_sim PRIVATE ray_core ray_kernel ray_cam ray_lib)
