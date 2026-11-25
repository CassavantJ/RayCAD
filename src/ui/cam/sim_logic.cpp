#include "sim_logic.h"

#include <QtCore/QCoreApplication>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace ray::ui::camui {

namespace {

QString trs(const char* text)
{
    return QCoreApplication::translate("ray::ui::SimToolModel", text);
}

constexpr double kSpeeds[] = {1, 2, 5, 10, 25, 50, 100, 250, 1000};

// sim.h EventKind, in its order.
constexpr const char* kKindNames[] = {"RapidIntoStock",
                                      "RapidIntoCutStock",
                                      "HolderHitsStock",
                                      "ShankHitsStock",
                                      "ToolHitsFixture",
                                      "HolderHitsFixture",
                                      "HolderHitsPart",
                                      "Gouge",
                                      "PlungeWithNonCentreCutting",
                                      "ExcessEngagement",
                                      "FeedIntoStockAtRapidHeight",
                                      "TravelLimit",
                                      "ToolTooShort",
                                      "ShankHitsPart",
                                      "FixtureHitsPart",
                                      "FixtureHitsStock"};

} // namespace

std::uint32_t stockTriangleBudget(double simCellMm)
{
    if (!(simCellMm > 0) || !std::isfinite(simCellMm))
        return 250'000;
    // 150,000 at 0.2 mm, and 100,000 more for each halving of the cell (see the header).
    const double budget = std::clamp(150'000.0 + 100'000.0 * std::log2(0.2 / simCellMm), 100'000.0, 400'000.0);
    return static_cast<std::uint32_t>(std::lround(budget / 1000.0) * 1000);
}

std::uint32_t playbackCap(std::uint32_t budget)
{
    return std::max<std::uint32_t>(10'000, budget / 2);
}

std::span<const double> playbackSpeeds()
{
    return kSpeeds;
}

QString speedText(double multiple)
{
    return QStringLiteral("%1×").arg(multiple, 0, 'g', 6);
}

const char* eventKindName(int kind)
{
    if (kind < 0 || kind >= static_cast<int>(std::size(kKindNames)))
        return "";
    return kKindNames[kind];
}

QString eventTitle(int kind)
{
    switch (kind) {
    case 0:
        return trs("Rapid move into material");
    case 1:
        return trs("Rapid move into material left by earlier cuts");
    case 2:
        return trs("Holder hits the stock");
    case 3:
        return trs("Tool shank rubs the stock");
    case 4:
        return trs("Tool hits a fixture");
    case 5:
        return trs("Holder hits a fixture");
    case 6:
        return trs("Holder hits the part");
    case 7:
        return trs("Gouge: cuts into the finished part");
    case 8:
        return trs("Straight plunge with a tool that does not cut at its centre");
    case 9:
        return trs("Heavy cut: more tool engagement than planned");
    case 10:
        return trs("Feed move enters material from rapid height");
    case 11:
        return trs("Past the machine's travel");
    case 12:
        return trs("Tool sticks out too little");
    case 13:
        return trs("Tool shank hits the part");
    case 14:
        return trs("Fixture cuts into the part");
    case 15:
        return trs("Fixture overlaps the stock");
    default:
        return trs("Problem");
    }
}

double seekBudgetMs(double simCellMm)
{
    // PLAN.md section 9, "Timeline seek": Low 250 ms, Medium 150, High 100 (by the preset's cell, as the simulator's
    // keyframe budget goes: src/sim/simulator.h keyframeBudgetFor).
    if (!(simCellMm > 0) || !std::isfinite(simCellMm))
        return 150;
    return simCellMm >= 0.15 ? 250 : simCellMm >= 0.075 ? 150 : 100;
}

QString slowSeekText(bool thinned, double worstSeekMs, double simCellMm)
{
    if (!thinned || !std::isfinite(worstSeekMs) || !(worstSeekMs > seekBudgetMs(simCellMm)))
        return {};
    // To a tenth of a second below ten seconds (never shown as 0.0: it is over the budget), whole seconds above.
    const double sec = worstSeekMs / 1000.0;
    const QString amount = sec < 9.95 ? QString::number(std::max(0.1, std::round(sec * 10.0) / 10.0), 'f', 1)
                                      : QString::number(std::llround(sec));
    return trs("Jumping around this program may take up to about %1 s at this resolution: it needs more memory for "
               "jump points than is allowed. A coarser resolution jumps faster.")
        .arg(amount);
}

QString severityKey(SimSeverity s)
{
    switch (s) {
    case SimSeverity::Error:
        return QStringLiteral("error");
    case SimSeverity::Warning:
        return QStringLiteral("warning");
    case SimSeverity::Info:
        break;
    }
    return QStringLiteral("info");
}

std::vector<MarkerCluster> clusterMarkers(std::span<const MarkerInput> markers, double mergePx)
{
    std::vector<MarkerCluster> out;
    double lastX = 0;
    for (const MarkerInput& m : markers) {
        if (!out.empty() && m.x - lastX <= mergePx) {
            MarkerCluster& c = out.back();
            c.events.push_back(m.event);
            c.severity = std::max(c.severity, m.severity);
        } else {
            MarkerCluster c;
            c.x = m.x;
            c.severity = m.severity;
            c.events.push_back(m.event);
            out.push_back(std::move(c));
        }
        lastX = m.x;
    }
    return out;
}

QString formatClock(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0)
        seconds = 0;
    if (seconds >= 3600) {
        const auto whole = static_cast<long long>(std::llround(seconds));
        return QStringLiteral("%1:%2:%3")
            .arg(whole / 3600)
            .arg((whole / 60) % 60, 2, 10, QLatin1Char('0'))
            .arg(whole % 60, 2, 10, QLatin1Char('0'));
    }
    const auto tenths = static_cast<long long>(std::llround(seconds * 10));
    const long long minutes = tenths / 600;
    const long long rest = tenths % 600;
    return QStringLiteral("%1:%2.%3").arg(minutes).arg(rest / 10, 2, 10, QLatin1Char('0')).arg(rest % 10);
}

} // namespace ray::ui::camui
