# store: Native file, objects, commits, recovery journal, migrations. Allowed dependencies: PLAN.md section 1.1.
add_library(ray_store STATIC
    store_module.cpp
    file.cpp
    file_db.cpp
    file_util.cpp
    lock.cpp
    lock_posix.cpp
    lock_win.cpp
    objects.cpp
)
ray_module(ray_store DETERMINISTIC)
target_link_libraries(ray_store PRIVATE ray_core ray_doc)

# SQLite (the container) and Zstandard (object compression), both private to this module (PLAN.md 4.1).
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
target_link_libraries(ray_store PRIVATE unofficial::sqlite3::sqlite3 zstd::libzstd)
