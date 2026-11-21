# sketch: Sketch entities, constraints, solver, DXF. Allowed dependencies: PLAN.md section 1.1.
add_library(ray_sketch STATIC
    sketch_module.cpp
    model.h model.cpp
    solver_internal.h solver_system.h solver.cpp
    solver_translate.h solver_translate.cpp
    solver_iterate.h solver_iterate.cpp
    solver_diagnose.h solver_diagnose.cpp
    solver_drag.h solver_drag.cpp
    loops.h loops.cpp loops_curves.h loops_curves.cpp
    infer.cpp
    dxf.h dxf_read.cpp dxf_write.cpp)
ray_module(ray_sketch DETERMINISTIC)
target_link_libraries(ray_sketch PRIVATE ray_core ray_kernel)

# PlaneGCS (FreeCAD 1.1.4, LGPL-2.1-or-later), vendored unmodified with a small shim; see third_party/planegcs.
add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/planegcs" "${PROJECT_BINARY_DIR}/third_party/planegcs")
target_link_libraries(ray_sketch PRIVATE ray_planegcs)
