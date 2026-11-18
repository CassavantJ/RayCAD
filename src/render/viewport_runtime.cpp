// Qt runtime for programs without a Qt application of their own, and PNG encoding (task T-014).
#include <ray_render/viewport.h>

#include <QtCore/QBuffer>
#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QtGlobal>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ray::render {

namespace {

// argc and argv must outlive the application.
int g_argc = 1;
char g_arg0[] = "raycad";
char* g_argv[] = {g_arg0, nullptr};

// Destroyed at exit by destroyApplication. Exit handlers run in reverse order of registration, and the OpenGL
// driver registers its own when it is loaded, after the application was created. A static destructor for the
// application therefore ran after Mesa had shut llvmpipe down, and Qt's GL teardown then crashed (Qt offscreen
// platform, Linux). So the handler is registered again once a driver was loaded (noteGraphicsDriverLoaded), which
// makes it run before the driver's.
QGuiApplication* g_application = nullptr;

void destroyApplication()
{
    delete g_application; // the second registration finds nothing left to do
    g_application = nullptr;
}

std::mutex g_warningsMutex;
std::vector<std::string> g_warnings;
QtMessageHandler g_previousHandler = nullptr;

void recordMessage(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
        std::lock_guard lock(g_warningsMutex);
        g_warnings.push_back(message.toStdString());
    }
    if (g_previousHandler)
        g_previousHandler(type, context, message);
}

} // namespace

std::vector<std::string> takeQtWarnings()
{
    std::lock_guard lock(g_warningsMutex);
    return std::exchange(g_warnings, {});
}

Status ensureQtRuntime()
{
    if (QCoreApplication::instance()) {
        if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
            return tl::unexpected(
                Error{ErrorCode::Internal, "A QCoreApplication exists; the viewport needs a QGuiApplication", {}});
        return {};
    }
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    // No display server: render offscreen (Mesa's llvmpipe on CI).
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM") && !qEnvironmentVariableIsSet("DISPLAY") &&
        !qEnvironmentVariableIsSet("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
    // QRhi then warns about resources still alive when it is destroyed.
    if (!qEnvironmentVariableIsSet("QT_RHI_LEAK_CHECK"))
        qputenv("QT_RHI_LEAK_CHECK", "1");
    g_previousHandler = qInstallMessageHandler(recordMessage);
    g_application = new QGuiApplication(g_argc, g_argv);
    std::atexit(destroyApplication);
    return {};
}

namespace detail {
void noteGraphicsDriverLoaded()
{
    static std::once_flag once;
    if (g_application)
        std::call_once(once, [] { std::atexit(destroyApplication); });
}
} // namespace detail

int processEventsFor(int ms)
{
    if (!QCoreApplication::instance())
        return 0;
    QEventLoop loop;
    int ticks = 0;
    QTimer tick;
    tick.setInterval(5);
    QObject::connect(&tick, &QTimer::timeout, [&ticks] { ++ticks; });
    tick.start();
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
    return ticks;
}

bool processEventsUntil(const std::function<bool()>& done, int timeoutMs)
{
    if (!QCoreApplication::instance())
        return done();
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs) {
        QEventLoop loop;
        QTimer::singleShot(5, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return done();
}

Result<std::vector<std::byte>> encodePng(const Image& image)
{
    if (image.width <= 0 || image.height <= 0 ||
        image.rgba.size() != std::size_t(image.width) * std::size_t(image.height) * 4)
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "Image size does not match its pixels", {}});
    const QImage view(image.rgba.data(), image.width, image.height, image.width * 4, QImage::Format_RGBA8888);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!view.save(&buffer, "PNG"))
        return tl::unexpected(Error{ErrorCode::Internal, "PNG encoding failed", {}});
    std::vector<std::byte> out(std::size_t(bytes.size()));
    std::memcpy(out.data(), bytes.constData(), out.size());
    return out;
}

Result<Image> decodePng(std::span<const std::byte> png)
{
    QImage decoded;
    if (!decoded.loadFromData(QByteArrayView(reinterpret_cast<const char*>(png.data()), qsizetype(png.size())), "PNG"))
        return tl::unexpected(Error{ErrorCode::FormatError, "Not a readable PNG image", {}});
    const QImage rgba = decoded.convertToFormat(QImage::Format_RGBA8888);
    Image image;
    image.width = rgba.width();
    image.height = rgba.height();
    image.rgba.resize(std::size_t(image.width) * std::size_t(image.height) * 4);
    for (int y = 0; y < image.height; ++y)
        std::memcpy(&image.rgba[std::size_t(y) * std::size_t(image.width) * 4], rgba.constScanLine(y),
                    std::size_t(image.width) * 4);
    return image;
}

Result<Image> readPng(const std::string& path)
{
    QFile file(QString::fromUtf8(path.c_str()));
    if (!file.open(QIODevice::ReadOnly))
        return tl::unexpected(Error{ErrorCode::IoError, "Cannot read " + path, {}});
    const QByteArray bytes = file.readAll();
    return decodePng(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.constData()), std::size_t(bytes.size())));
}

Status writePng(const std::string& path, const Image& image)
{
    auto png = encodePng(image);
    if (!png)
        return tl::unexpected(png.error());
    QFile file(QString::fromUtf8(path.c_str()));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(reinterpret_cast<const char*>(png->data()), qint64(png->size())) != qint64(png->size()))
        return tl::unexpected(Error{ErrorCode::IoError, "Cannot write " + path, {}});
    return {};
}

} // namespace ray::render
