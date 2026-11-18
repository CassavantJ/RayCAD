# render: tier detection, theme tokens, viewport renderer. Qt is allowed here and in src/ui only.
find_package(Qt6 6.12 REQUIRED COMPONENTS Gui)

add_library(ray_render STATIC render_module.cpp)
ray_module(ray_render)
target_link_libraries(ray_render PRIVATE ray_core ray_kernel ray_sim Qt6::Gui)
