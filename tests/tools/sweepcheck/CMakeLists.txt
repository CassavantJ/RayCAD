# The 2D cutter-sweep checker (task T-060; sweepcheck.h): spike S5's checker as a test library. It links nothing of
# RayCAD's: it must stay independent of the toolpath generators it measures.
add_library(ray_sweepcheck STATIC
    s5_cases.h # the S5 test pockets (test data, shared with the adaptive engine's tests)
    sweepcheck.cpp
    sweepcheck.h
)
target_include_directories(ray_sweepcheck PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(ray_sweepcheck PUBLIC cxx_std_20)
set_property(TARGET ray_sweepcheck PROPERTY FOLDER "tests")

# Its own tests: S5's self-test (measured engagement of straight and arc passes against the formulas), and deliberately
# bad toolpaths it must catch (gouge, uncut area, overload, a link cutting material, air cutting).
add_executable(ray_sweepcheck_tests sweepcheck_test.cpp)
target_link_libraries(ray_sweepcheck_tests PRIVATE ray_sweepcheck Catch2::Catch2WithMain)
ray_add_test(ray_sweepcheck_tests)
