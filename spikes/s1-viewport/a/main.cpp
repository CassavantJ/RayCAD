// Spike S1, variant A: own renderer on Qt RHI (QQuickRhiItem). Direct3D 11 by default on Windows.
#include "bench.h"
#include "scene.h"
#include "viewport.h"
#include <QQmlApplicationEngine>
#include <QQuickGraphicsConfiguration>
#include <QScreen>
#include <QSurfaceFormat>

int main(int argc, char **argv)
{
    const double mainStartMs = s1::msSinceProcessStart();
    s1::clock();
    s1::installCrashTrace();
    QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
    fmt.setSwapInterval(0);   // vsync off: Qt Quick then sets QRhiSwapChain::NoVSync
    const bool gl = qgetenv("QSG_RHI_BACKEND") == "opengl";
    if (gl) { fmt.setVersion(4, 5); fmt.setProfile(QSurfaceFormat::CoreProfile); }
    QSurfaceFormat::setDefaultFormat(fmt);

    QGuiApplication app(argc, argv);
    const s1::Options opt = s1::parseOptions(app);
    qmlRegisterType<ViewportItem>("S1", 1, 0, "Viewport");
    qmlRegisterType<BackdropItem>("S1", 1, 0, "Backdrop");

    QQmlApplicationEngine engine;
    engine.setInitialProperties({{"glass", opt.glass}, {"msaa", opt.msaa}, {"flipY", gl ? 1.0 : 0.0}});
    engine.load(QUrl("qrc:/qml/MainA.qml"));
    if (engine.rootObjects().isEmpty()) return 2;
    auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    auto *vp = win->findChild<ViewportItem *>("viewport");
    if (opt.gpuTime) {
        QQuickGraphicsConfiguration cfg = win->graphicsConfiguration();
        cfg.setTimestamps(true);
        win->setGraphicsConfiguration(cfg);
    }
    const qreal dpr = win->screen()->devicePixelRatio();
    win->setPosition(40, 40);
    win->resize(qRound(opt.width / dpr), qRound(opt.height / dpr));

    s1::Bench bench(win, opt, mainStartMs);
    bench.extra["app"] = "A";
    bench.pickTest = [vp](s1::Bench::Done done) { s1::runPickTest(vp, done); };
    win->show();
    return app.exec();
}
