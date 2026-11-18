#include <ray_ui/app.h>

#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuick/QQuickWindow>

#include <cstdio>

namespace ray::ui {

int runApp(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("RayCAD"));
    QGuiApplication::setApplicationVersion(QStringLiteral(RAY_VERSION_STRING));

    const QStringList arguments = QGuiApplication::arguments();
    if (arguments.contains(QStringLiteral("--version"))) {
        std::printf("raycad %s\n", RAY_VERSION_STRING);
        return 0;
    }
    const bool smoke = arguments.contains(QStringLiteral("--smoke"));

    QQmlApplicationEngine engine;
    engine.loadFromModule("RayCad.Ui", "Main");
    if (engine.rootObjects().isEmpty()) {
        std::fprintf(stderr, "raycad: the main window failed to load\n");
        return 2;
    }

    if (smoke) {
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
        if (window == nullptr) {
            std::fprintf(stderr, "raycad: the root QML object is not a window\n");
            return 2;
        }
        QObject::connect(
            window, &QQuickWindow::frameSwapped, &app,
            [] {
                std::printf("raycad: smoke ok, first frame rendered\n");
                std::fflush(stdout);
                QCoreApplication::exit(0);
            },
            Qt::QueuedConnection);
        QTimer::singleShot(20000, &app, [] {
            std::fprintf(stderr, "raycad: smoke failed, no frame was rendered within 20 seconds\n");
            QCoreApplication::exit(3);
        });
    }
    return QGuiApplication::exec();
}

} // namespace ray::ui
