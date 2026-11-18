// Command-line options of the desktop app (see ray_ui/app.h for what each one does).
#pragma once

#include <QtCore/QPointF>
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
    QString theme;                   // "" = default (follow the OS)
    std::optional<bool> glass;       // unset = default (flat until T-024 reads the tier preset)
    QString screenshot;              // PNG path; empty = none
    QSize size;                      // window size in logical pixels; invalid = default
    QStringList files;               // files to open (T-020: the first STEP/IGES file, through DevOpen)
    std::optional<QPointF> selectAt; // --select-at X,Y: click-select there (window coordinates) before a screenshot
    // --dev-session (hidden, T-021 developer option until T-019 wires the real session): run with an api::makeApp
    // session, so the command palette lists the registry's actions. (Qt itself takes -session and --session.)
    bool session = false;
    QString error; // a usage error; non-empty means "print and exit 1"
};

AppOptions parseOptions(const QStringList& arguments); // arguments()[0] is the program and is skipped
QString usageText();

} // namespace ray::ui
