# ui: the Qt Quick application. Qt is allowed here and in src/render only.
# GuiPrivate: ray_render links it, and the static QML plugin walks ray_render's dependencies, so it must be found here
# too. The QML imports QtQuick.Templates (controls are styled from scratch, no Qt Quick Controls style is loaded)
# QtQuick.Shapes (icons, the view cube) and QtQuick.Dialogs (the native open, save and export dialogs); they load
# from the Qt installation at run time.
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui GuiPrivate Qml Quick)
qt_standard_project_setup(REQUIRES 6.8)
# tests/apps needs the Qt DLL folder on Windows and must not call find_package(Qt6) itself.
set_property(GLOBAL PROPERTY RAY_QT_BIN_DIR "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")

add_library(ray_ui STATIC
    ui_module.cpp
    include/ray_ui/app.h
    app.cpp
    app_clock.h
    app_clock.cpp
    app_options.h
    app_options.cpp
    app_host.h
    app_host.cpp
    scene_jobs.h
    session_recovery.h
    session_recovery.cpp
    viewport_navigation.h
    viewport_navigation.cpp
    palette_schema.h
    palette_schema.cpp)
ray_module(ray_ui)
target_include_directories(ray_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# ray_api brings ray_api/viewer.h: the viewer's kernel and naming queries (src/ui may link only api and render).
target_link_libraries(ray_ui PRIVATE ray_api ray_render Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick)

# QML is compiled ahead of time by qmlcachegen, the default of qt_add_qml_module. qmltc is NOT used: it does
# not build with Qt Quick Controls in Qt 6.12.0 (spike S2).
# The C++ singletons Theme (theme_object.h), AppContext (app_context.h), Scene (scene_controller.h), Selection
# (selection_model.h), Session (session.h), Files (files.h), Commands (palette_commands.h), Shortcuts
# (shortcuts.h), MeasureTool (measure_model.h, T-022), AppSettings (settings.h) and Units (units.h), and the
# ViewportItem (viewport_item.h) and PaletteModel (palette_model.h) types, are registered declaratively through
# QML_SINGLETON / QML_NAMED_ELEMENT in the headers listed under SOURCES.
set(RAY_UI_QML_FILES
    qml/Main.qml
    qml/Shell.qml
    qml/StartScreen.qml
    qml/Gallery.qml
    qml/Viewport.qml
    qml/ViewCube.qml
    qml/SelectionFilterBar.qml
    qml/CommandHost.qml
    qml/CommandPalette.qml
    qml/ContextMenu.qml
    qml/panels/PanelBase.qml
    qml/panels/ModelTree.qml
    qml/panels/SelectionInfo.qml
    qml/panels/JobsPanel.qml
    qml/panels/Measure.qml
    qml/panels/MeasureOverlay.qml
    qml/dialogs/DialogBase.qml
    qml/dialogs/UnsavedChangesDialog.qml
    qml/dialogs/RecoveryDialog.qml
    qml/dialogs/ReadOnlyNotice.qml
    qml/dialogs/FileDialogs.qml
    qml/dialogs/FileMenu.qml
    qml/panels/ShortcutSettings.qml
    qml/panels/Settings.qml
    qml/controls/icons.js
    qml/controls/Icon.qml
    qml/controls/IconText.qml
    qml/controls/FocusRing.qml
    qml/controls/Label.qml
    qml/controls/Button.qml
    qml/controls/ToolButton.qml
    qml/controls/TextField.qml
    qml/controls/NumberField.qml
    qml/controls/CheckBox.qml
    qml/controls/Switch.qml
    qml/controls/ComboBox.qml
    qml/controls/Slider.qml
    qml/controls/ProgressBar.qml
    qml/controls/TabBar.qml
    qml/controls/TabButton.qml
    qml/controls/ToolTip.qml
    qml/controls/HoverToolTip.qml
    qml/controls/Menu.qml
    qml/controls/MenuItem.qml
    qml/controls/MenuSeparator.qml
    qml/controls/ScrollBar.qml
    qml/controls/TreeRow.qml
    qml/controls/SplitterHandle.qml
    qml/controls/PanelHeader.qml)
qt_add_qml_module(ray_ui
    URI RayCad.Ui
    VERSION 1.0
    OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/qml/RayCad/Ui"
    SOURCES
        theme_object.h
        theme_object.cpp
        app_context.h
        app_context.cpp
        viewport_item.h
        viewport_item.cpp
        scene_controller.h
        scene_controller.cpp
        selection_model.h
        selection_model.cpp
        session.h
        session.cpp
        files.h
        files.cpp
        palette_commands.h
        palette_commands.cpp
        palette_model.h
        palette_model.cpp
        shortcuts.h
        shortcuts.cpp
        settings.h
        settings.cpp
        units.h
        units.cpp
        measure_model.h
        measure_model.cpp
    QML_FILES ${RAY_UI_QML_FILES}
    DEPENDENCIES
        QtQuick
        QtQuick.Templates
        QtQuick.Shapes
        QtQuick.Dialogs)

# The C++ that qmlcachegen generates from the QML is Qt's code, not ours: its warnings (MSVC C4702 "unreachable
# code" inside Qt's inline headers) do not fail the build. Our own sources keep warnings as errors.
foreach(_qml IN LISTS RAY_UI_QML_FILES)
    string(REGEX REPLACE "[/.]" "_" _generated "${_qml}")
    set(_generated "${CMAKE_CURRENT_BINARY_DIR}/.rcc/qmlcache/ray_ui_${_generated}.cpp")
    if(MSVC)
        set_source_files_properties("${_generated}" PROPERTIES COMPILE_OPTIONS "/wd4702;/WX-")
    else()
        set_source_files_properties("${_generated}" PROPERTIES COMPILE_OPTIONS "-Wno-error")
    endif()
endforeach()

# The shipped themes, compiled in under :/ray/themes/ (ThemeObject loads every *.json there).
file(GLOB RAY_UI_THEME_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/resources/themes/*.json")
qt_add_resources(ray_ui "ray_ui_themes"
    PREFIX "/ray/themes"
    BASE "${CMAKE_SOURCE_DIR}/resources/themes"
    FILES ${RAY_UI_THEME_FILES})

# ray_ui is a static library, so the QML module's resources and type registration live in the static plugin
# ray_uiplugin that qt_add_qml_module generates. Whoever links ray_ui must link that plugin too, or the linker
# drops them and the QML is not found at run time. INTERFACE hands it to the executable, which then does not
# have to name anything from Qt itself.
target_link_libraries(ray_ui INTERFACE ray_uiplugin)
