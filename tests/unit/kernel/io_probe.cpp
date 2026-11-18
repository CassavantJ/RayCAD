// Imports one STEP or IGES file and prints what came out (task T-005). tools/import_corpus.py runs it once per
// file in a child process, so a crash or a hang is caught and attributed to one file.
//
// Usage: ray_kernel_io_probe <file> [--heal none|reader|full] [--no-sew] [--bodies] [--cancel-stages]
//   --bodies         one line per body (name, colour, counts, volume)
//   --cancel-stages  after the normal import, import again once per stage and cancel half-way through that
//                    stage; prints the time from cancel() to the return of the import call
//   --cancel-sweep N also import N more times, cancelling at N evenly spaced points of the normal import's time
// Stage names as the caller sees them are rate-limited by ray::Progress (30 Hz), so a short stage can be
// missing from the timeline; fraction_at_cancel tells the stage too (see io_import.cpp for the stage ranges).
//
// Output: plain "key=value" lines. The summary line starts with "result=". Exit code 0 when the import
// succeeded, 1 when it returned an error, 2 for bad arguments. Anything else is a crash.
#include <ray/kernel.h>

#include <io_internal.h>
#include <shape_testing.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace ray;
using namespace ray::kernel;

namespace {

using Clock = std::chrono::steady_clock;

double msBetween(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

const char* codeName(ErrorCode code)
{
    switch (code) {
    case ErrorCode::Cancelled:
        return "Cancelled";
    case ErrorCode::InvalidArgument:
        return "InvalidArgument";
    case ErrorCode::NotFound:
        return "NotFound";
    case ErrorCode::IoError:
        return "IoError";
    case ErrorCode::FormatError:
        return "FormatError";
    case ErrorCode::FormatTooNew:
        return "FormatTooNew";
    case ErrorCode::KernelFailure:
        return "KernelFailure";
    case ErrorCode::Internal:
        return "Internal";
    default:
        return "Other";
    }
}

// Progress as the caller sees it: every (time, fraction, stage) the listener got.
struct Timeline {
    std::mutex mutex;
    Clock::time_point start = Clock::now();
    struct Point {
        double ms;
        double fraction;
        std::string stage;
    };
    std::vector<Point> points;
    Progress progress()
    {
        return Progress([this](double fraction, std::string_view stage) {
            std::lock_guard<std::mutex> lock(mutex);
            points.push_back(Point{msBetween(start, Clock::now()), fraction, std::string(stage)});
        });
    }
    std::string lastStage()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return points.empty() ? std::string("(none yet)") : points.back().stage;
    }
};

struct Options {
    std::string path;
    ImportOptions import;
    bool iges = false;
    bool bodies = false;
    bool cancelStages = false;
    int cancelSweep = 0;
};

Result<ImportResult> runImport(const Options& options, JobContext& ctx)
{
    return options.iges ? importIges(options.path, options.import, ctx) : importStep(options.path, options.import, ctx);
}

// Imports again and cancels `atMs` after the start; prints the time from cancel() to the return of the call,
// the stage and fraction the caller had last seen, and how long the abandoned work then took to end.
void cancelAt(const Options& options, double atMs, const std::string& label)
{
    CancelSource source;
    Timeline timeline;
    JobContext cancellable{source.token(), timeline.progress()};
    std::atomic<std::int64_t> cancelledAt{0};
    std::string stageAtCancel;
    double fractionAtCancel = 0.0;
    std::thread canceller([&] {
        std::this_thread::sleep_until(timeline.start + std::chrono::duration<double, std::milli>(atMs));
        {
            std::lock_guard<std::mutex> lock(timeline.mutex);
            stageAtCancel = timeline.points.empty() ? std::string("(none yet)") : timeline.points.back().stage;
            fractionAtCancel = timeline.points.empty() ? 0.0 : timeline.points.back().fraction;
        }
        cancelledAt = Clock::now().time_since_epoch().count();
        source.cancel();
    });
    const Result<ImportResult> cancelled = runImport(options, cancellable);
    const std::int64_t returned = Clock::now().time_since_epoch().count();
    canceller.join();
    const double latencyMs =
        std::chrono::duration<double, std::milli>(Clock::duration(returned - cancelledAt.load())).count();
    const char* outcome = cancelled.has_value()                            ? "finished-first"
                          : cancelled.error().code == ErrorCode::Cancelled ? "cancelled"
                                                                           : codeName(cancelled.error().code);
    const Clock::time_point drainStart = Clock::now();
    kernel::detail::waitForIoThreads();
    std::printf("cancel stage=\"%s\" at_ms=%.1f stage_at_cancel=\"%s\" fraction_at_cancel=%.3f outcome=%s "
                "latency_ms=%.2f abandoned_work_ended_after_ms=%.1f\n",
                label.c_str(), atMs, stageAtCancel.c_str(), fractionAtCancel, outcome, latencyMs,
                msBetween(drainStart, Clock::now()));
    std::fflush(stdout);
}

std::string lower(std::string text)
{
    for (char& c : text)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return text;
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--heal" && i + 1 < argc) {
            const std::string mode = argv[++i];
            options.import.heal = mode == "none" ? Heal::None : mode == "reader" ? Heal::Reader : Heal::Full;
        } else if (arg == "--no-sew") {
            options.import.sew = false;
        } else if (arg == "--bodies") {
            options.bodies = true;
        } else if (arg == "--cancel-stages") {
            options.cancelStages = true;
        } else if (arg == "--cancel-sweep" && i + 1 < argc) {
            options.cancelSweep = std::max(0, std::atoi(argv[++i]));
        } else if (!arg.empty() && arg[0] != '-' && options.path.empty()) {
            options.path = arg;
        } else {
            std::fprintf(stderr, "usage: ray_kernel_io_probe <file> [--heal none|reader|full] [--no-sew] [--bodies] "
                                 "[--cancel-stages]\n");
            return 2;
        }
    }
    if (options.path.empty()) {
        std::fprintf(stderr, "usage: ray_kernel_io_probe <file> [options]\n");
        return 2;
    }
    const std::string lowered = lower(options.path);
    options.iges = lowered.ends_with(".igs") || lowered.ends_with(".iges");

    Timeline timeline;
    JobContext ctx{CancelToken(), timeline.progress()};
    const Clock::time_point start = Clock::now();
    const Result<ImportResult> result = runImport(options, ctx);
    const double totalMs = msBetween(start, Clock::now());

    // Stage start times as the caller saw them.
    std::vector<Timeline::Point> stages;
    bool monotonic = true;
    double lastFraction = 0.0;
    for (const Timeline::Point& point : timeline.points) {
        monotonic = monotonic && point.fraction >= lastFraction;
        lastFraction = point.fraction;
        if (stages.empty() || stages.back().stage != point.stage)
            stages.push_back(point);
    }
    for (std::size_t i = 0; i < stages.size(); ++i) {
        const double end = i + 1 < stages.size() ? stages[i + 1].ms : totalMs;
        std::printf("stage name=\"%s\" start_ms=%.1f duration_ms=%.1f\n", stages[i].stage.c_str(), stages[i].ms,
                    end - stages[i].ms);
    }

    if (!result.has_value()) {
        std::printf("result=error code=%s time_ms=%.1f message=\"%s\"\n", codeName(result.error().code), totalMs,
                    result.error().message.c_str());
        std::fflush(stdout);
        return 1;
    }

    int solids = 0, validSolids = 0, faces = 0, named = 0, colored = 0, invalid = 0;
    std::set<std::array<float, 4>> colors;
    for (std::size_t i = 0; i < result->bodies.size(); ++i) {
        const ImportedBody& body = result->bodies[i];
        const auto bodySolids = static_cast<int>(body.shape.count(SubKind::Solid));
        const auto bodyFaces = static_cast<int>(body.shape.count(SubKind::Face));
        const Result<bool> valid = testing::isValidShape(body.shape);
        const bool isValid = valid.has_value() && *valid;
        solids += bodySolids;
        faces += bodyFaces;
        if (isValid)
            validSolids += bodySolids;
        else
            ++invalid;
        if (body.name.rfind("Body ", 0) != 0)
            ++named;
        const std::array<float, 4> grey{0.75f, 0.75f, 0.75f, 1.0f};
        if (body.color != grey)
            ++colored;
        colors.insert(body.color);
        if (options.bodies) {
            const Result<MassProps> props = massProps(body.shape);
            std::printf(
                "body index=%zu name=\"%s\" color=%.3f,%.3f,%.3f,%.3f solids=%d faces=%d valid=%d volume=%.9g\n", i,
                body.name.c_str(), body.color[0], body.color[1], body.color[2], body.color[3], bodySolids, bodyFaces,
                isValid ? 1 : 0, props.has_value() ? props->volume : -1.0);
        }
    }
    for (const std::string& warning : result->warnings)
        std::printf("warning \"%s\"\n", warning.c_str());
    std::printf("result=ok time_ms=%.1f bodies=%zu solids=%d valid_solids=%d invalid_bodies=%d faces=%d units=%s "
                "named=%d colored=%d distinct_colors=%zu warnings=%zu progress_monotonic=%d progress_end=%.3f\n",
                totalMs, result->bodies.size(), solids, validSolids, invalid, faces,
                result->sourceUnits == UnitSystem::Inch ? "inch" : "metric", named, colored, colors.size(),
                result->warnings.size(), monotonic ? 1 : 0,
                timeline.points.empty() ? 0.0 : timeline.points.back().fraction);
    std::fflush(stdout);

    if (options.cancelStages) {
        // One cancel per stage name, half-way through its longest stretch.
        std::vector<std::size_t> chosen;
        for (std::size_t i = 0; i < stages.size(); ++i) {
            const auto length = [&](std::size_t k) {
                return (k + 1 < stages.size() ? stages[k + 1].ms : totalMs) - stages[k].ms;
            };
            if (stages[i].stage == "Done" || length(i) < 20.0)
                continue;
            const auto same = std::find_if(chosen.begin(), chosen.end(),
                                           [&](std::size_t k) { return stages[k].stage == stages[i].stage; });
            if (same == chosen.end())
                chosen.push_back(i);
            else if (length(i) > length(*same))
                *same = i;
        }
        for (const std::size_t i : chosen) {
            const double end = i + 1 < stages.size() ? stages[i + 1].ms : totalMs;
            cancelAt(options, (stages[i].ms + end) / 2.0, stages[i].stage);
        }
    }
    for (int k = 0; k < options.cancelSweep; ++k)
        cancelAt(options, totalMs * (k + 0.5) / options.cancelSweep, "sweep");
    return 0;
}
