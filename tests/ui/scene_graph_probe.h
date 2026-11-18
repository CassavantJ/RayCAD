// Shared by the Qt Quick tests in tests/ui: whether the scene graph renders through a QRhi, so that a viewport can
// draw and answer picks. Asked of a real, shown window, once per process: QQuickWindow::graphicsApi() only says what
// would be requested, and on a platform without OpenGL (the offscreen platform on Linux CI and
// tools/linux-local.sh) Qt Quick falls back to its software backend afterwards ("No QRhi found for window").
// Requires a QGuiApplication.
#pragma once

#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRendererInterface>
#include <QtTest/QTest>

#include <catch2/catch_test_macros.hpp>

namespace ray::uitest {

inline bool rhiSceneGraph()
{
    static const bool rhi = [] {
        QQuickWindow probe;
        probe.resize(64, 64);
        probe.show();
        if (!QTest::qWaitForWindowExposed(&probe))
            return false;
        (void)probe.grabWindow(); // initialises the scene graph
        const QSGRendererInterface* renderer = probe.rendererInterface();
        return renderer != nullptr && QSGRendererInterface::isApiRhiBased(renderer->graphicsApi());
    }();
    return rhi;
}

} // namespace ray::uitest

// Skips the current test case, with a message, where the viewport cannot render.
#define RAY_REQUIRE_RHI()                                                                                              \
    do {                                                                                                               \
        if (!::ray::uitest::rhiSceneGraph())                                                                           \
            SKIP("the scene graph has no QRhi (software backend): the viewport cannot render here");                   \
    } while (false)
