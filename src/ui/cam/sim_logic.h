// The non-visual part of the Simulate workspace (task T-074; the panel is sim_tool.h), tested without a window
// (tests/ui/cam/sim_unit_test.cpp). Each choice here is a shop-practice judgment, written down where it is made:
//
//   Budget     the stock's display triangle budget from the tier preset's simulation cell (a preset field, never the
//              tier's name): 150,000 at Low's 0.2 mm and 100,000 more for each halving of the cell, so 250,000 at
//              Medium's 0.1 and 350,000 at High's 0.05 (about 86 bytes a triangle on the GPU: 13, 22 and 30 MB). What
//              bounds it is not GPU memory but the frame that brings it: Qt Quick's render thread packs and uploads a
//              mesh while the UI thread waits in the scene graph's sync, measured at about 65 ns a triangle on the
//              reference machine (a whole 0.05 mm stock at 1.6 million triangles held the UI thread 64 to 114 ms), so
//              the whole display must come in one frame within the UI thread's budget (section 9: 50 ms on Low,
//              which is about 3.5 times slower, 33 ms on High). The simulation itself runs at the preset's cell; only
//              the display merges columns (IStockModel::toMesh). During playback a dirty region is asked for with at
//              most half the budget (playbackCap): its tiles may come coarser, and the whole display is asked for
//              again at the full budget when playback stops.
//   Speeds     machine-time multiples: 1x (the program as the machine would run it, for watching one move), 2x, 5x,
//              10x (the default: a ten-minute program in a minute), 25x, 50x, 100x, 250x and 1000x (an hour-long
//              program in under 15 s). "One move per step" is the step keys' job, not a speed. When the simulation
//              cannot keep up, the clock keeps its speed and the stock catches up to where the clock is (frames are
//              dropped, never time).
//   Severity   as the event says (sim.h SimEvent::severity): Error, Warning or Info. Every check of T-071 and the gouge
//              test of T-072 raise Error; the fixture checks (after T-074) raise Error for a fixture in the part and
//              Warning for one in the stock only; ExcessEngagement and FeedIntoStockAtRapidHeight would be Warning when
//              raised. The panel adds nothing and drops nothing.
//   Setup      an event of the setup (sim.h kSetupEvent: a fixture against the part or the stock) has no move: its row
//              says "Before the first move", its marker sits at the start of the timeline, choosing it goes to the
//              start and marks the place of the overlap on the stock.
//   Seeks      when a run's keyframes had to be thinned and its slowest seek is estimated over the tier's budget
//              (seekBudgetMs), one plain line says so near the resolution (slowSeekText); nothing otherwise.
//   Words      each kind's title in a machinist's words (eventTitle); the event's own message follows it unchanged
//              (with its resolution where it depends on it).
//   Markers    events closer than kMarkerMergePx on the timeline merge into one marker (clusterMarkers): it sits at the
//              earliest of them, takes the worst severity and shows how many; a click goes to the earliest.
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <cstdint>
#include <span>
#include <vector>

namespace ray::ui::camui {

// ---------- Display budget ----------
std::uint32_t stockTriangleBudget(double simCellMm);
std::uint32_t playbackCap(std::uint32_t budget);
// The GPU bytes a triangle of the stock takes (vertex and index data, render/rhi/stock_pool: about 86).
inline constexpr double kStockBytesPerTriangle = 86.0;

// ---------- Speeds ----------
// The speeds offered, as multiples of machine time, slowest first.
std::span<const double> playbackSpeeds();
inline constexpr int kDefaultSpeedIndex = 3; // 10x
QString speedText(double multiple);          // "10×"

// ---------- Events ----------
enum class SimSeverity : std::uint8_t { Info, Warning, Error };
// The kind's name as sim.h EventKind's enumerator ("HolderHitsPart"), for tests and diagnostics; "" when unknown.
const char* eventKindName(int kind);
// A machinist's title for an event kind (sim.h EventKind as an int).
QString eventTitle(int kind);
// "error", "warning", "info".
QString severityKey(SimSeverity s);

// ---------- Seek time (added with the slow-seek notice) ----------
// The seek budget of the tier whose preset cell this is (PLAN.md section 9: Low 250 ms, Medium 150, High 100).
double seekBudgetMs(double simCellMm);
// The panel's line when a run's seeks will be slow (sim::SeekReport): only when its keyframes were thinned (memory is
// the cause the line names) and the estimate is over the budget; "" otherwise. The time to a tenth of a second.
QString slowSeekText(bool thinned, double worstSeekMs, double simCellMm);

// ---------- Markers ----------
inline constexpr double kMarkerMergePx = 8.0;
struct MarkerInput {
    double x = 0; // pixels along the track
    SimSeverity severity = SimSeverity::Error;
    int event = 0; // index of the event
};
struct MarkerCluster {
    double x = 0; // where the marker sits (its earliest event)
    SimSeverity severity = SimSeverity::Info;
    std::vector<int> events; // in time order
};
// `markers` sorted by x. Each marker joins the cluster before it when it lies within `mergePx` of that cluster's last
// marker, so a run of close events stays one marker however long it is.
std::vector<MarkerCluster> clusterMarkers(std::span<const MarkerInput> markers, double mergePx = kMarkerMergePx);

// ---------- Times ----------
// A time on the timeline: "0:07.4", "12:03.0", "1:02:03" (tenths below an hour).
QString formatClock(double seconds);

} // namespace ray::ui::camui
