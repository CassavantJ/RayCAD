# core: Ids, hashing, Result, units, deterministic math, job system, logging. Allowed dependencies: PLAN.md section 1.1.
add_library(ray_core STATIC core_module.cpp)
ray_module(ray_core DETERMINISTIC)

# Job system (T-003) on oneTBB, linked dynamically.
find_package(TBB CONFIG REQUIRED)
target_sources(ray_core PRIVATE jobs.cpp)
target_link_libraries(ray_core PRIVATE TBB::tbb)
