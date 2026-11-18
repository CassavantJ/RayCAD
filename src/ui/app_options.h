// Command-line options of the desktop app (see ray_ui/app.h for what each one does).
#pragma once

#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <optional>

namespace ray::ui {

struct AppOptions {
    bool smoke = false;
    bool benchStart = false;
    bool gallery = false;
    bool version = false;
    bool help = false;
    QString theme;             // "" = default (follow the OS)
    std::optional<bool> glass; // unset = default (flat until T-024 reads the tier preset)
    QString screenshot;        // PNG path; empty = none
    QSize size;                // window size in logical pixels; invalid = default
    QString error;             // a usage error; non-empty means "print and exit 1"
};

AppOptions parseOptions(const QStringList& arguments); // arguments()[0] is the program and is skipped
QString usageText();

} // namespace ray::ui
