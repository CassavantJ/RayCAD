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
    queued_call.h
    session_recovery.h
    session_recovery.cpp
    viewport_navigation.h
    viewport_navigation.cpp
    palette_schema.h
    palette_schema.cpp
    sketch/sketch_geom.h
    sketch/sketch_geom.cpp
    sketch/sketch_tools.h
    sketch/sketch_tools.cpp
    sketch/sketch_drag.h
    sketch/sketch_drag.cpp
    sketch/dimensions.h
    sketch/dimensions.cpp
    sketch/constraints.h
    sketch/constraints.cpp
    model/model_util.h
    model/preview.h
    model/preview.cpp
    model/directedit_logic.h # T-083
    model/directedit_logic.cpp
    cam/setup_logic.h # T-044
    cam/setup_logic.cpp
    cam/operation_logic.h # T-045
    cam/operation_logic.cpp
    cam/tools_logic.h # T-046
    cam/tools_logic.cpp
    cam/post_logic.h # T-047
    cam/post_logic.cpp
    cam/cycletime_logic.h # T-059
    cam/cycletime_logic.cpp
    cam/setupsheet_print.h # T-076
    cam/setupsheet_print.cpp
    cam/fixtures_logic.h # T-077
    cam/fixtures_logic.cpp
    cam/sim_logic.h # T-074
    cam/sim_logic.cpp
    cam/sim_carried.h
    cam/sim_carried.cpp)
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
    qml/panels/Section.qml
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
    qml/controls/ScrollEdges.qml
    qml/controls/Menu.qml
    qml/controls/MenuItem.qml
    qml/controls/MenuSeparator.qml
    qml/controls/ScrollBar.qml
    qml/controls/TreeRow.qml
    qml/controls/SplitterHandle.qml
    qml/controls/PanelHeader.qml
    qml/sketch/SketchLayer.qml
    qml/sketch/SketchPanel.qml
    qml/sketch/SketchGlyph.qml
    qml/sketch/DimensionLayer.qml
    qml/sketch/ConstraintsLayer.qml
    qml/sketch/ConstraintsPanel.qml
    qml/model/ModelToolButtons.qml
    qml/model/FeaturePanel.qml
    qml/model/FeatureLayer.qml
    qml/model/FeatureRow.qml
    qml/model/ParameterTable.qml
    qml/model/DirectEditPanel.qml # T-083
    qml/model/DirectEditLayer.qml
    qml/panels/ReferenceReview.qml
    qml/model/ReplaceImport.qml # T-084
    qml/cam/SetupPanel.qml
    qml/cam/CamTree.qml
    qml/cam/SchemaForm.qml
    qml/cam/OperationPanel.qml
    qml/cam/ToolLibrary.qml # T-046
    qml/cam/ToolProfile.qml
    qml/cam/ToolFields.qml
    qml/cam/ToolPicker.qml
    qml/cam/PostDialog.qml # T-047
    qml/cam/GcodeView.qml
    qml/cam/Templates.qml # T-057
    qml/cam/SetupSheetDialog.qml # T-076
    qml/cam/Fixtures.qml # T-077
    qml/cam/SimPanel.qml # T-074
    qml/cam/Timeline.qml)
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
        ui_commands.h
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
        section_model.h
        section_model.cpp
        sketch/sketch_mode.h
        sketch/sketch_mode.cpp
        sketch/sketch_overlay.h
        sketch/sketch_overlay.cpp
        sketch/constraints_rows.h
        sketch/constraints_rows.cpp
        sketch/constraints_model.h
        sketch/constraints_model.cpp
        sketch/dimensions_overlay.h
        sketch/dimensions_overlay.cpp
        model/feature_tools.h
        model/feature_tools.cpp
        model/feature_tree.h
        model/feature_tree.cpp
        model/params.h
        model/params.cpp
        model/directedit_tool.h # T-083
        model/directedit_tool.cpp
        model/directedit_review.h
        model/directedit_review.cpp
        model/replace_import.h # T-084
        model/replace_import.cpp
        cam/setup_tool.h
        cam/setup_tool.cpp
        cam/setup_tree.h
        cam/setup_tree.cpp
        cam/operation_form.h
        cam/operation_form.cpp
        cam/operation_tool.h
        cam/operation_tool.cpp
        cam/operation_view.h
        cam/operation_view.cpp
        cam/tools_model.h # T-046
        cam/tools_model.cpp
        cam/post_view.h # T-047
        cam/post_view.cpp
        cam/post_tool.h
        cam/post_tool.cpp
        cam/templates_model.h # T-057
        cam/templates_model.cpp
        cam/setupsheet_tool.h # T-076
        cam/setupsheet_tool.cpp
        cam/fixtures_tool.h # T-077
        cam/fixtures_tool.cpp
        cam/sim_tool.h # T-074
        cam/sim_tool.cpp
    QML_FILES ${RAY_UI_QML_FILES}
    DEPENDENCIES
        QtQuick
        QtQuick.Templates
        QtQuick.Shapes
        QtQuick.Dialogs)
# The generated type registrations include QML_ELEMENT headers by file name: sketch/ (T-092) must be on the path.
target_include_directories(ray_ui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/sketch")
target_include_directories(ray_ui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/model") # T-098, likewise
target_include_directories(ray_ui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/cam")   # T-044, likewise

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
# T-076: the setup sheet picture's print palette (white background, dark edges), under :/ray/print/ so it is not one of
# the themes the user picks (ThemeObject loads :/ray/themes/ only). Only IViewport::captureImageInTheme uses it.
qt_add_resources(ray_ui "ray_ui_print_theme"
    PREFIX "/ray/print"
    BASE "${CMAKE_SOURCE_DIR}/resources/themes/print"
    FILES "${CMAKE_SOURCE_DIR}/resources/themes/print/setupsheet-print.json")

# ray_ui is a static library, so the QML module's resources and type registration live in the static plugin
# ray_uiplugin that qt_add_qml_module generates. Whoever links ray_ui must link that plugin too, or the linker
# drops them and the QML is not found at run time. INTERFACE hands it to the executable, which then does not
# have to name anything from Qt itself.
target_link_libraries(ray_ui INTERFACE ray_uiplugin)
