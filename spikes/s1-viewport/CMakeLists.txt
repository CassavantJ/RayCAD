cmake_minimum_required(VERSION 3.24)
project(s1_viewport LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui GuiPrivate Qml Quick ShaderTools OpenGL)
find_package(OpenCASCADE CONFIG REQUIRED)

set(S1_QML qml/MainA.qml qml/MainB.qml qml/Panel.qml)
set(S1_PANEL_SHADER shaders/panel.frag)

# ---- A: own renderer on Qt RHI (QQuickRhiItem) ----
qt_add_executable(s1a a/main.cpp a/viewport.cpp a/viewport.h common/scene.h common/bench.h)
target_include_directories(s1a PRIVATE common)
target_link_libraries(s1a PRIVATE Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::Qml Qt6::Quick)
qt_add_resources(s1a "qml_a" PREFIX "/" FILES ${S1_QML})
qt_add_shaders(s1a "shaders_a" PREFIX "/" GLSL "120,150" HLSL 50 MSL 12 FILES
    shaders/scene.vert shaders/scene.frag shaders/id.frag shaders/line.vert shaders/line.frag
    shaders/quad.vert shaders/down.frag shaders/blur.frag ${S1_PANEL_SHADER})

# ---- B: OCCT V3d_View in a QQuickFramebufferObject, Qt Quick forced to OpenGL ----
qt_add_executable(s1b b/main.cpp b/occtview.cpp b/occtview.h common/scene.h common/bench.h)
target_include_directories(s1b PRIVATE common ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(s1b PRIVATE Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::Qml Qt6::Quick Qt6::OpenGL
    TKernel TKMath TKG3d TKService TKV3d TKOpenGl)
qt_add_resources(s1b "qml_b" PREFIX "/" FILES ${S1_QML})
qt_add_shaders(s1b "shaders_b" PREFIX "/" GLSL "120,150" HLSL 50 MSL 12 FILES ${S1_PANEL_SHADER})

# console subsystem so the result line reaches stdout
set_target_properties(s1a s1b PROPERTIES WIN32_EXECUTABLE FALSE)

# debug information for the crash trace aid (S1_CRASH_TRACE=1)
target_compile_options(s1a PRIVATE /Zi)
target_compile_options(s1b PRIVATE /Zi)
target_link_options(s1a PRIVATE /DEBUG /OPT:REF /OPT:ICF)
target_link_options(s1b PRIVATE /DEBUG /OPT:REF /OPT:ICF)
target_link_libraries(s1a PRIVATE dbghelp)
target_link_libraries(s1b PRIVATE dbghelp)
