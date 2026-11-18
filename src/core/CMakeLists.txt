# core: Ids, hashing, Result, units, deterministic math, job system, logging. Allowed dependencies: PLAN.md section 1.1.
add_library(ray_core STATIC core_module.cpp)
ray_module(ray_core DETERMINISTIC)
