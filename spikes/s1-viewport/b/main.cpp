// Spike S1, variant B: OCCT V3d_View in a QQuickFramebufferObject, Qt Quick forced to the OpenGL backend.
#include "bench.h"
#include "occtview.h"
#include <QQmlApplicationEngine>
#include <QQuickGraphicsConfiguration>
#include <QScreen>
#include <QSurfaceFormat>

int main(int argc, char **argv)
{
    const double mainStartMs = s1::msSinceProcessStart();
    s1::clock();
    s1::installCrashTrace();
    qputenv("QSG_RHI_BACKEND", "opengl");   // QQuickFramebufferObject only works on OpenGL
    QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
    fmt.setSwapInterval(0);
    fmt.setVersion(4, 5);
    fmt.setProfile(qEnvironmentVariableIntValue("S1_GL_COMPAT") ? QSurfaceFormat::CompatibilityProfile : QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(24);
    fmt.setStencilBufferSize(8);
    QSurfaceFormat::setDefaultFormat(fmt);

    QGuiApplication app(argc, argv);
    const s1::Options opt = s1::parseOptions(app);
    qmlRegisterType<OcctView>("S1", 1, 0, "OcctView");

    QQmlApplicationEngine engine;
    engine.setInitialProperties({{"glass", opt.glass}, {"msaa", opt.msaa}, {"flipY", 0.0}});
    engine.load(QUrl("qrc:/qml/MainB.qml"));
    if (engine.rootObjects().isEmpty()) return 2;
    auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    auto *vp = win->findChild<OcctView *>("viewport");
    if (opt.gpuTime) {
        QQuickGraphicsConfiguration cfg = win->graphicsConfiguration();
        cfg.setTimestamps(true);
        win->setGraphicsConfiguration(cfg);
    }
    const qreal dpr = win->screen()->devicePixelRatio();
    win->setPosition(40, 40);
    win->resize(qRound(opt.width / dpr), qRound(opt.height / dpr));

    s1::Bench bench(win, opt, mainStartMs);
    bench.extra["app"] = "B";
    bench.pickTest = [vp](s1::Bench::Done done) { s1::runPickTest(vp, done); };
    win->show();
    return app.exec();
}
