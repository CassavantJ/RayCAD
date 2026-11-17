cmake_minimum_required(VERSION 3.24)
set(VCPKG_APPLOCAL_DEPS OFF) # the deploy step below copies the DLLs explicitly
project(s2_startup LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)
find_package(OpenCASCADE CONFIG REQUIRED)
qt_standard_project_setup(REQUIRES 6.8)

option(S2_QMLTC "Also build the qmltc variant (shell_qmltc)" OFF)

# OCCT toolkits the product will use: modelling, booleans, fillets, offset, healing, meshing, STEP/IGES with XCAF.
set(S2_OCCT_LIBS
    TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKGeomAlgo TKTopAlgo TKPrim
    TKBO TKBool TKFillet TKOffset TKShHealing TKMesh
    TKLCAF TKCAF TKXCAF TKXSBase TKDE TKDESTEP TKDEIGES)

file(GLOB S2_QML_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS qml/*.qml)
foreach(f ${S2_QML_FILES})
    get_filename_component(n ${f} NAME)
    set_source_files_properties(${f} PROPERTIES QT_RESOURCE_ALIAS ${n})
endforeach()
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

# QML module compiled ahead of time by qmlcachegen (the default of qt_add_qml_module), shared by all variants.
qt_add_qml_module(rayshell_qml STATIC
    URI RayShell
    VERSION 1.0
    QML_FILES ${S2_QML_FILES})

function(s2_add_shell name)
    qt_add_executable(${name} WIN32 src/main.cpp ${ARGN})
    target_link_libraries(${name} PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 rayshell_qmlplugin)
    target_link_options(${name} PRIVATE /INCREMENTAL:NO /OPT:REF /OPT:ICF)
endfunction()

# (1) baseline: OCCT DLLs are ordinary imports, loaded by the OS loader before main().
s2_add_shell(shell_base src/occt_touch.cpp)
target_include_directories(shell_base PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(shell_base PRIVATE ${S2_OCCT_LIBS})

# (2) delay-load: same code, every OCCT DLL is /DELAYLOAD, so it loads on first call into it.
s2_add_shell(shell_delay src/occt_touch.cpp)
target_include_directories(shell_delay PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(shell_delay PRIVATE ${S2_OCCT_LIBS} delayimp)
foreach(lib ${S2_OCCT_LIBS})
    target_link_options(shell_delay PRIVATE /DELAYLOAD:${lib}.dll)
endforeach()

# Reference: the same shell with no OCCT at all (lower bound for what delay-loading can reach).
s2_add_shell(shell_noocct)
target_compile_definitions(shell_noocct PRIVATE S2_NO_OCCT)

set(S2_EXES shell_base shell_delay shell_noocct)

if(S2_QMLTC)
    add_subdirectory(qmltc)
    list(APPEND S2_EXES shell_qmltc)
endif()

# Self-contained deploy folder, as an installer would lay it out: build/deploy
set(DEPLOY_DIR ${CMAKE_BINARY_DIR}/deploy)
get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
get_filename_component(_qtbin ${_qmake} DIRECTORY)
set(_copy_exes)
foreach(e ${S2_EXES})
    list(APPEND _copy_exes COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${e}> ${DEPLOY_DIR})
endforeach()
add_custom_target(deploy ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${DEPLOY_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DEPLOY_DIR}
    ${_copy_exes}
    COMMAND ${_qtbin}/windeployqt.exe --release --no-compiler-runtime --no-translations
            --qmldir ${CMAKE_CURRENT_SOURCE_DIR}/qml --dir ${DEPLOY_DIR} ${DEPLOY_DIR}/shell_base.exe
    COMMAND ${CMAKE_COMMAND} -DEXE=${DEPLOY_DIR}/shell_base.exe -DDEST=${DEPLOY_DIR}
            -DSEARCH=${OpenCASCADE_BINARY_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/copy_deps.cmake
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/qml ${DEPLOY_DIR}/qml_loose
    COMMAND ${CMAKE_COMMAND} -E rename ${DEPLOY_DIR}/qml_loose/loose.qmldir ${DEPLOY_DIR}/qml_loose/qmldir
    DEPENDS ${S2_EXES}
    VERBATIM)
