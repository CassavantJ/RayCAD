# The desktop executable. It depends on ui only and names no Qt target itself.
add_executable(raycad main.cpp)
target_link_libraries(raycad PRIVATE ray_ui)
set_property(TARGET raycad PROPERTY FOLDER "apps")
