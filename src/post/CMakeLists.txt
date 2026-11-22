# post: Post engine, G-code output, setup sheet. Allowed dependencies: PLAN.md section 1.1.
add_library(ray_post STATIC
    post_module.cpp
    format.h format.cpp
    writer.h writer.cpp
    engine.h engine.cpp
    engine_host.h engine_host.cpp
    engine_out.cpp
    engine_program.h engine_program.cpp
    engine_motion.h engine_motion.cpp
    engine_thread.cpp)
ray_module(ray_post DETERMINISTIC)
target_link_libraries(ray_post PRIVATE ray_core ray_cam ray_lib)

# QuickJS-NG 0.18 from the overlay port ports/quickjs-ng: the plain qjs library (no quickjs-libc), with the
# interrupt poll patched to every 100 back-edges (THIRD_PARTY.md). Static; only src/post includes quickjs.h.
# (The post thread's pthread_create needs no library of its own: glibc 2.34 and later and macOS have it in libc,
# and qjs already links the platform's thread library.)
find_package(qjs CONFIG REQUIRED)
target_link_libraries(ray_post PRIVATE qjs)
