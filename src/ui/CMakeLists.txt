# ui: the Qt Quick application. Qt is allowed here and in src/render only.
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Qml Quick)
qt_standard_project_setup(REQUIRES 6.8)
# tests/apps needs the Qt DLL folder on Windows and must not call find_package(Qt6) itself.
set_property(GLOBAL PROPERTY RAY_QT_BIN_DIR "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")

add_library(ray_ui STATIC ui_module.cpp app.cpp include/ray_ui/app.h)
ray_module(ray_ui)
target_include_directories(ray_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ray_ui PRIVATE ray_api ray_render Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick)

# QML is compiled ahead of time by qmlcachegen, the default of qt_add_qml_module. qmltc is NOT used: it does
# not build with Qt Quick Controls in Qt 6.12.0 (spike S2).
qt_add_qml_module(ray_ui
    URI RayCad.Ui
    VERSION 1.0
    OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/qml/RayCad/Ui"
    QML_FILES qml/Main.qml)

# ray_ui is a static library, so the QML module's resources and type registration live in the static plugin
# ray_uiplugin that qt_add_qml_module generates. Whoever links ray_ui must link that plugin too, or the linker
# drops them and the QML is not found at run time. INTERFACE hands it to the executable, which then does not
# have to name anything from Qt itself.
target_link_libraries(ray_ui INTERFACE ray_uiplugin)
