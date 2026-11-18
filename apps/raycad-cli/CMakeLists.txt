# The headless executable for tests and golden files. It depends on api only: no Qt, no window.
add_executable(raycad-cli main.cpp)
target_link_libraries(raycad-cli PRIVATE ray_api)
set_property(TARGET raycad-cli PROPERTY FOLDER "apps")
