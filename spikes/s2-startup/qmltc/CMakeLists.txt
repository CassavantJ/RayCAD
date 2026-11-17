# qmltc attempt: the same QML files compiled to C++ by the QML type compiler.
# Enabled with -DS2_QMLTC=ON. See REPORT.md for the result.
find_package(Qt6 REQUIRED COMPONENTS QmlPrivate QuickPrivate QuickControls2Private QuickTemplates2Private QuickLayoutsPrivate)
file(GLOB TC_QML_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../qml/*.qml)
foreach(f ${TC_QML_FILES})
    get_filename_component(n ${f} NAME)
    set_source_files_properties(${f} PROPERTIES QT_RESOURCE_ALIAS ${n})
endforeach()
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/../qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_executable(shell_qmltc WIN32 ../src/main.cpp ../src/occt_touch.cpp)
target_compile_definitions(shell_qmltc PRIVATE S2_QMLTC)
target_include_directories(shell_qmltc PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(shell_qmltc PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    Qt6::QmlPrivate Qt6::QuickPrivate Qt6::QuickControls2Private Qt6::QuickTemplates2Private Qt6::QuickLayoutsPrivate
    ${S2_OCCT_LIBS} delayimp)
foreach(lib ${S2_OCCT_LIBS})
    target_link_options(shell_qmltc PRIVATE /DELAYLOAD:${lib}.dll)
endforeach()
target_link_options(shell_qmltc PRIVATE /INCREMENTAL:NO /OPT:REF /OPT:ICF)
qt_add_qml_module(shell_qmltc
    URI RayShellTc
    VERSION 1.0
    QML_FILES ${TC_QML_FILES}
    ENABLE_TYPE_COMPILER)
