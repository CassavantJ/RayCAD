#include "app_options.h"

namespace ray::ui {

AppOptions parseOptions(const QStringList& arguments)
{
    AppOptions options;
    auto value = [&](qsizetype& i, const QString& name) -> QString {
        if (i + 1 >= arguments.size()) {
            options.error = QStringLiteral("%1 needs a value").arg(name);
            return {};
        }
        return arguments.at(++i);
    };
    for (qsizetype i = 1; i < arguments.size() && options.error.isEmpty(); ++i) {
        const QString& arg = arguments.at(i);
        if (arg == QLatin1String("--smoke")) {
            options.smoke = true;
        } else if (arg == QLatin1String("--bench-start") || arg == QLatin1String("--bench-startup")) {
            options.benchStart = true;
        } else if (arg == QLatin1String("--gallery")) {
            options.gallery = true;
        } else if (arg == QLatin1String("--version")) {
            options.version = true;
        } else if (arg == QLatin1String("--help") || arg == QLatin1String("-h")) {
            options.help = true;
        } else if (arg == QLatin1String("--theme")) {
            options.theme = value(i, arg);
        } else if (arg == QLatin1String("--glass")) {
            const QString v = value(i, arg).toLower();
            if (v == QLatin1String("on")) {
                options.glass = true;
            } else if (v == QLatin1String("off")) {
                options.glass = false;
            } else if (options.error.isEmpty()) {
                options.error = QStringLiteral("--glass takes on or off");
            }
        } else if (arg == QLatin1String("--screenshot")) {
            options.screenshot = value(i, arg);
        } else if (arg == QLatin1String("--size")) {
            const QStringList parts = value(i, arg).split(QLatin1Char('x'));
            bool okW = false;
            bool okH = false;
            const int w = parts.size() == 2 ? parts[0].toInt(&okW) : 0;
            const int h = parts.size() == 2 ? parts[1].toInt(&okH) : 0;
            if (okW && okH && w >= 320 && h >= 240) {
                options.size = QSize(w, h);
            } else if (options.error.isEmpty()) {
                options.error = QStringLiteral("--size takes WIDTHxHEIGHT, at least 320x240");
            }
        } else if (arg.startsWith(QLatin1String("--"))) {
            options.error = QStringLiteral("unknown option %1").arg(arg);
        }
        // Anything else (a file to open) is for T-019.
    }
    return options;
}

QString usageText()
{
    return QStringLiteral("Usage: raycad [options]\n"
                          "  --theme <name>      system (default), dark, light, or a theme id such as graphite-dark\n"
                          "  --glass on|off      glass or flat panels\n"
                          "  --gallery           show the controls gallery instead of the shell\n"
                          "  --size WxH          window size in logical pixels\n"
                          "  --screenshot <png>  save the window after it settles, then exit\n"
                          "  --smoke             exit 0 after the first rendered frame\n"
                          "  --bench-start       print the time from process creation to the first frame, then exit\n"
                          "  --version           print the version\n");
}

} // namespace ray::ui
