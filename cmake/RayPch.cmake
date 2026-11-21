# Precompiled headers (RAY_PCH, on by default): the standard library, tl::expected, Qt and Catch2, which every
# translation unit of a target parses again otherwise. Only stable third-party headers, never a RayCAD header, so
# editing RayCAD code never rebuilds a precompiled header.
#
# Which targets: the Qt targets (ray_ui with its qmlcachegen and moc sources, ray_render), ray_api and the test
# executables, where most of the compile time goes. Not the deterministic modules (PLAN.md section 8: core, kernel,
# naming, sketch, doc, store and the CAM modules, and PlaneGCS): a precompiled header puts its headers in front of
# every source, and there the order and configuration of what a source includes is left exactly as written. Not
# ray_header_tests, which proves each public header compiles on its own.
#
# A precompiled header can hide a missing #include. The linux-clang-asan preset builds with RAY_PCH=OFF, so CI
# compiles every source without one on each pull request.
option(RAY_PCH "Precompile the heavy third-party headers (standard library, Qt, Catch2)" ON)

set(RAY_PCH_STD
    <algorithm> <array> <atomic> <chrono> <cmath> <cstddef> <cstdint> <cstdio> <cstdlib> <cstring> <functional>
    <limits> <map> <memory> <mutex> <optional> <set> <string> <string_view> <thread> <tuple> <unordered_map>
    <utility> <variant> <vector>
    <tl/expected.hpp>)
set(RAY_PCH_QT_CORE
    <QtCore/QObject> <QtCore/QString> <QtCore/QStringList> <QtCore/QVariant> <QtCore/QVariantMap> <QtCore/QPointer>
    <QtCore/QByteArray> <QtCore/QTimer> <QtCore/QDir> <QtCore/QJsonObject> <QtCore/QJsonArray> <QtCore/QJsonDocument>
    <QtGui/QColor> <QtGui/QGuiApplication> <QtGui/QImage>)
set(RAY_PCH_QT_QUICK
    <QtGui/QKeyEvent> <QtGui/QMouseEvent> <QtQml/QQmlEngine> <QtQml/qqmlprivate.h> <QtQuick/QQuickItem>
    <QtQuick/QQuickWindow>)
set(RAY_PCH_CATCH2 <catch2/catch_test_macros.hpp>)

# ray_precompile(<target> <header>...)
function(ray_precompile target)
    if(RAY_PCH AND TARGET ${target})
        target_precompile_headers(${target} PRIVATE ${ARGN})
    endif()
endfunction()

# Called at the end of the top-level CMakeLists.txt, once every target exists.
function(ray_precompile_all)
    ray_precompile(ray_api ${RAY_PCH_STD})
    ray_precompile(ray_render ${RAY_PCH_STD} ${RAY_PCH_QT_CORE} <rhi/qrhi.h>)
    ray_precompile(ray_ui ${RAY_PCH_STD} ${RAY_PCH_QT_CORE} ${RAY_PCH_QT_QUICK})

    # Tests with several translation units that include no Qt header (the render tests do not either: ray_render's
    # interface has none). A test of one translation unit, or of a few small ones, gains nothing: the precompiled
    # header costs about what it saves and its compile comes first (measured: the one-file Qt Quick tests, the core,
    # job and module tests).
    foreach(_test IN ITEMS
            ray_api_tests ray_doc_tests ray_doc_s4_tests ray_kernel_tests ray_naming_tests ray_naming_oracle_tests
            ray_render_tests ray_sketch_tests ray_store_tests)
        ray_precompile(${_test} ${RAY_PCH_STD} ${RAY_PCH_CATCH2})
    endforeach()
    # The one Qt Quick test with several translation units.
    ray_precompile(ray_ui_sketch_tests ${RAY_PCH_STD} ${RAY_PCH_QT_CORE} ${RAY_PCH_QT_QUICK} <QtTest/QTest>
                   ${RAY_PCH_CATCH2})
endfunction()
