#include "cycletime_logic.h"

#include <QtCore/QCoreApplication>

#include <cmath>

namespace ray::ui::camui {

namespace {
QString trc(const char* text)
{
    return QCoreApplication::translate("ray::ui::camui::CycleTime", text);
}
} // namespace

QString formatDuration(double seconds)
{
    if (!(seconds >= 0) || !std::isfinite(seconds) || seconds > 1e12)
        return {};
    const long long total = std::llround(seconds);
    const long long h = total / 3600, m = (total / 60) % 60, s = total % 60;
    return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
}

QString cycleTimeShort(double seconds)
{
    const QString d = formatDuration(seconds);
    return d.isEmpty() ? d : trc("est. %1").arg(d);
}

QStringList assumptionNotes(const cam::CycleTimeAssumptions& a)
{
    // The values are cam.h's kCycleTimeDefault* constants.
    QStringList out;
    if (a.accelerationUnknown)
        out << trc("Machine accelerations unknown: %1 mm/s² assumed.").arg(cam::kCycleTimeDefaultAccelMmS2);
    if (a.rapidUnknown)
        out << trc("Machine rapid rates unknown: %1 mm/min assumed.").arg(cam::kCycleTimeDefaultRapidMmMin);
    if (a.maxFeedUnknown)
        out << trc("Machine maximum feeds unknown: feeds held to the rapid rate.");
    if (a.toolChangeUnknown)
        out << trc("Tool change time unknown: %1 s assumed.").arg(cam::kCycleTimeDefaultToolChangeSec);
    if (a.feedMissing)
        out << trc("A feed move has no feed: the feed before it was used.");
    return out;
}

QString cycleTimeDetails(const cam::CycleTimePart& p, const cam::CycleTimeAssumptions& assumptions, bool complete)
{
    const QString total = formatDuration(p.totalSec);
    if (total.isEmpty())
        return {};
    QStringList parts;
    parts << trc("cutting %1").arg(formatDuration(p.cutSec)) << trc("rapids %1").arg(formatDuration(p.rapidSec));
    if (p.dwellSec > 0)
        parts << trc("dwells %1").arg(formatDuration(p.dwellSec));
    if (p.toolChanges > 0)
        parts << (p.toolChanges == 1
                      ? trc("1 tool change %1").arg(formatDuration(p.toolChangeSec))
                      : trc("%1 tool changes %2").arg(p.toolChanges).arg(formatDuration(p.toolChangeSec)));
    QString text = trc("Estimated cycle time %1 (%2).").arg(total, parts.join(QStringLiteral(", ")));
    if (!complete)
        text += QLatin1Char(' ') + trc("Only the generated toolpaths are counted.");
    for (const QString& note : assumptionNotes(assumptions))
        text += QLatin1Char('\n') + note;
    return text;
}

} // namespace ray::ui::camui
