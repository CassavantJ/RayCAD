# RAY_TEST_SCREEN (test_screen.h): keeps the windows of the tests off the primary screen. ray_ui_test_screen is linked
# into every test that shows a window, with the listener test_screen_listener.cpp (tests/CMakeLists.txt); the
# raycad.exe that raycad_smoke starts is placed by tests/support/run_with_driver_priority.py instead.
# ray_ui_screen_tests checks the selection, the placement and the unset path, with no window.
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui GuiPrivate)

add_library(ray_ui_test_screen STATIC test_screen.h test_screen.cpp)
target_include_directories(ray_ui_test_screen PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# GuiPrivate: QWindowSystemInterface::handlePrimaryScreenChanged. dwmapi: the visible frame of a window.
target_link_libraries(ray_ui_test_screen PUBLIC Qt6::Core Qt6::Gui PRIVATE Qt6::GuiPrivate Catch2::Catch2)
if(WIN32)
    target_link_libraries(ray_ui_test_screen PRIVATE dwmapi)
endif()
set_property(TARGET ray_ui_test_screen PROPERTY FOLDER "tests")

add_executable(ray_ui_screen_tests screen_test.cpp)
target_link_libraries(ray_ui_screen_tests PRIVATE ray_ui_test_screen Catch2::Catch2WithMain)
set_property(TARGET ray_ui_screen_tests PROPERTY FOLDER "tests")
add_test(NAME ray_ui_screen_tests COMMAND ray_ui_screen_tests)
ray_test_runtime_path(ray_ui_screen_tests QT)
