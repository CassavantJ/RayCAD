// Success rates of the direct edits on the S9 set (task T-080, PLAN.md T-080 acceptance), measured by the spike's
// method (src/kernel/directedit_testing.h THE S9 METHOD) on tests/corpus/directedit/set.tsv: 11 NIST test parts (two of
// them fetch-only: tests/corpus/cache, tools/corpus_fetch.py) and 39 synthetic parts. A part that is not present is
// left out and named in the output; the rates are then on fewer parts.
//
// Every case runs with a time limit (kCaseLimit: 20 s, 200 s in a Debug build): a runaway Defeaturing call is
// cancelled, counted as a failure
// ("timeout"), and abandoned to the kernel thread it runs on. The cases run on kWorkers threads.
//
// RAY_DIRECTEDIT_RESULTS=<file> writes one tab-separated line per case (part, kind, edit, k, note, outcome, ms,
// message) for the report. RAY_DIRECTEDIT_ONLY=<part>:<edit>:<k> runs that one case alone and checks nothing (for
// profiling a slow case); RAY_DIRECTEDIT_LIMIT_S changes the time limit.
#include <catch2/catch_test_macros.hpp>

#include "kernel_test_util.h"

#include <directedit_testing.h>
#include <shape_testing.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace ray;
using namespace ray::kernel;
using namespace ray::kernel::test;

namespace {

using Clock = std::chrono::steady_clock;
constexpr int kWorkers = 8;

std::chrono::seconds caseLimit()
{
    const char* text = std::getenv("RAY_DIRECTEDIT_LIMIT_S");
#if defined(NDEBUG)
    constexpr int kDefault = 20;
#else
    constexpr int kDefault = 200; // unoptimised OpenCASCADE: ten times the Release limit
#endif
    return std::chrono::seconds(text != nullptr ? std::atoi(text) : kDefault);
}
const auto kCaseLimit = caseLimit();

std::string onlyCase()
{
    const char* text = std::getenv("RAY_DIRECTEDIT_ONLY");
    return text != nullptr ? std::string(text) : std::string();
}

struct Part {
    std::string name, path, kind;
    std::uintmax_t size = 0;
};

std::vector<Part> readSet(const std::filesystem::path& root)
{
    std::vector<Part> parts;
    std::ifstream in(root / "directedit" / "set.tsv");
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::vector<std::string> fields;
        std::stringstream split(line);
        std::string field;
        while (std::getline(split, field, '\t'))
            fields.push_back(field);
        if (fields.size() < 6)
            continue;
        parts.push_back({fields[0], fields[1], fields[2], std::stoull(fields[4])});
    }
    return parts;
}

// Cancels the sources of cases that ran past their deadline.
class Watchdog {
public:
    Watchdog() : thread_([this] { loop(); }) {}
    ~Watchdog()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }
    int add(CancelSource source, Clock::time_point deadline)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int id = next_++;
        watched_[id] = {std::move(source), deadline, {}};
        return id;
    }
    // When the source was cancelled for running late, if it was.
    std::optional<Clock::time_point> remove(int id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = watched_.find(id);
        std::optional<Clock::time_point> fired = it->second.fired;
        watched_.erase(it);
        return fired;
    }

private:
    struct Watched {
        CancelSource source;
        Clock::time_point deadline;
        std::optional<Clock::time_point> fired;
    };
    void loop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_) {
            wake_.wait_for(lock, std::chrono::milliseconds(20));
            const Clock::time_point now = Clock::now();
            for (auto& [id, w] : watched_)
                if (!w.fired && now >= w.deadline) {
                    w.source.cancel();
                    w.fired = now;
                }
        }
    }
    std::mutex mutex_;
    std::condition_variable wake_;
    std::map<int, Watched> watched_;
    int next_ = 0;
    bool stop_ = false;
    std::thread thread_;
};

struct Outcome {
    std::string part, kind, edit, note;
    int k = 0;
    std::string result;  // "ok", "refused" (KernelFailure), "bad_<verdict>" (success the oracle rejects), "timeout",
                         // "other_error"
    std::string message; // the kernel's message, or the oracle's detail
    double ms = 0.0;
    double cancelMs = -1.0;   // timeout: from the cancel to the call's return
    bool wrongCode = false;   // a failure that is not KernelFailure
    std::string historyFault; // a success whose history breaks the kernel.h normalisation (empty: none)
};

// kernel.h ShapeHistory for a direct edit: only input 0, every input element exactly one Modified or Deleted entry,
// results inside the result shape; a deleted face Deleted; a resized or moved face Modified (never Deleted plus a
// face from the tool).
std::string historyFault(const Shape& input, const testing::DirectEditCase& c, const OpResult& out)
{
    std::array<std::vector<int>, 4> primary;
    for (std::size_t k = 0; k < 4; ++k)
        primary[k].assign(input.count(static_cast<SubKind>(k)), 0);
    for (const HistoryEntry& e : out.history.entries) {
        if (e.inputIndex != 0)
            return "an entry names input " + std::to_string(e.inputIndex);
        const auto kind = static_cast<std::size_t>(e.source.kind);
        if (kind >= 4 || e.source.index >= primary[kind].size())
            return "an entry names an element the input does not have";
        for (const SubId& r : e.results)
            if (r.index >= out.shape.count(r.kind))
                return "a result is not in the result shape";
        if (e.relation != HistoryEntry::Relation::Generated)
            ++primary[kind][e.source.index];
    }
    for (const auto& perKind : primary)
        for (const int n : perKind)
            if (n != 1)
                return "an input element has " + std::to_string(n) + " Modified or Deleted entries";
    const bool deletes = c.edit != testing::DirectEdit::ResizeLarger && c.edit != testing::DirectEdit::ResizeSmaller &&
                         c.edit != testing::DirectEdit::PushOut && c.edit != testing::DirectEdit::PushIn;
    for (const SubId& f : c.faces)
        for (const HistoryEntry& e : out.history.entries)
            if (e.source == f && e.relation != HistoryEntry::Relation::Generated &&
                (e.relation == HistoryEntry::Relation::Deleted) != deletes)
                return std::string("edited face ") + std::to_string(f.index) +
                       (deletes ? " not Deleted" : " not Modified");
    return {};
}

Outcome runCase(const Part& part, const Shape& solid, const testing::DirectEditCase& c, Watchdog& watchdog)
{
    Outcome o;
    o.part = part.name;
    o.kind = part.kind;
    o.edit = testing::directEditName(c.edit);
    o.note = c.note;
    o.k = c.k;
    CancelSource source;
    JobContext ctx{source.token(), {}};
    const Clock::time_point start = Clock::now();
    const int watch = watchdog.add(source, start + kCaseLimit);
    Result<OpResult> result = tl::unexpected(Error{ErrorCode::Internal, "not run", {}});
    switch (c.edit) {
    case testing::DirectEdit::DeleteFillet:
    case testing::DirectEdit::DeleteBlendRun:
    case testing::DirectEdit::DeleteHole:
    case testing::DirectEdit::DeleteHoleStack:
    case testing::DirectEdit::DeletePocket:
        result = deleteFaces(solid, c.faces, ctx);
        break;
    case testing::DirectEdit::ResizeLarger:
    case testing::DirectEdit::ResizeSmaller:
        result = resizeHole(solid, c.faces.front(), c.newDiameter, ctx);
        break;
    case testing::DirectEdit::PushOut:
    case testing::DirectEdit::PushIn:
        result = offsetFaces(solid, c.faces, c.distance, ctx);
        break;
    }
    const Clock::time_point end = Clock::now();
    const std::optional<Clock::time_point> fired = watchdog.remove(watch);
    o.ms = std::chrono::duration<double, std::milli>(end - start).count();
    if (result) {
        const testing::DirectEditVerdict verdict = testing::judgeDirectEdit(solid, c, result->shape);
        o.result = verdict.category == "ok" ? "ok" : "bad_" + verdict.category;
        o.message = verdict.detail;
        if (o.result == "ok")
            o.historyFault = historyFault(solid, c, *result);
    } else if (result.error().code == ErrorCode::Cancelled && fired) {
        o.result = "timeout";
        o.cancelMs = std::chrono::duration<double, std::milli>(end - *fired).count();
        o.message = "stopped after " + std::to_string(kCaseLimit.count()) + " s";
    } else {
        o.result = result.error().code == ErrorCode::KernelFailure ? "refused" : "other_error";
        o.wrongCode = result.error().code != ErrorCode::KernelFailure;
        o.message = result.error().message;
    }
    return o;
}

std::string clean(std::string text)
{
    for (char& ch : text)
        if (ch == '\t' || ch == '\n' || ch == '\r')
            ch = ' ';
    return text;
}

struct Tally {
    int ok = 0, total = 0;
    double rate() const { return total == 0 ? 1.0 : static_cast<double>(ok) / total; }
};

} // namespace

TEST_CASE("direct edits on the S9 set meet the T-080 success rates", "[kernel][directedit_corpus]")
{
    const std::filesystem::path root(RAY_CORPUS_ROOT_DIR);
    const std::vector<Part> listed = readSet(root);
    REQUIRE(listed.size() == 50);
    std::vector<Part> parts;
    std::vector<std::string> missing;
    const std::string only = onlyCase();
    for (const Part& p : listed) {
        if (!only.empty() && only.rfind(p.name + ":", 0) != 0)
            continue;
        std::error_code ec;
        const auto size = std::filesystem::file_size(root / p.path, ec);
        if (ec)
            missing.push_back(p.name + " (" + p.path + ")");
        else {
            CHECK(size == p.size);
            parts.push_back(p);
        }
    }

    Watchdog watchdog;
    std::mutex mutex;
    std::vector<Outcome> outcomes;
    std::vector<std::string> loadFailures;
    std::atomic<std::size_t> nextPart{0};
    const Clock::time_point began = Clock::now();
    std::vector<std::thread> workers;
    for (int w = 0; w < kWorkers; ++w)
        workers.emplace_back([&] {
            for (std::size_t i = nextPart.fetch_add(1); i < parts.size(); i = nextPart.fetch_add(1)) {
                const Part& part = parts[i];
                JobContext ctx;
                auto imported = importStep((root / part.path).string(), {}, ctx);
                std::optional<Shape> solid;
                double largest = -1.0;
                if (imported)
                    for (const ImportedBody& body : imported->bodies)
                        if (body.shape.count(SubKind::Solid) == 1)
                            if (auto props = massProps(body.shape); props && props->volume > largest) {
                                largest = props->volume;
                                solid = body.shape;
                            }
                auto cases = solid ? testing::directEditCases(*solid)
                                   : Result<std::vector<testing::DirectEditCase>>(
                                         tl::unexpected(Error{ErrorCode::Internal, "no solid", {}}));
                if (!cases) {
                    std::lock_guard<std::mutex> lock(mutex);
                    loadFailures.push_back(part.name + ": " + cases.error().message);
                    continue;
                }
                for (const testing::DirectEditCase& c : *cases) {
                    if (!only.empty() &&
                        only != part.name + ":" + testing::directEditName(c.edit) + ":" + std::to_string(c.k))
                        continue;
                    Outcome o = runCase(part, *solid, c, watchdog);
                    std::lock_guard<std::mutex> lock(mutex);
                    outcomes.push_back(std::move(o));
                }
            }
        });
    for (std::thread& t : workers)
        t.join();
    const double seconds = std::chrono::duration<double>(Clock::now() - began).count();
    testing::waitForAbandonedEdits();
    const double withAbandoned = std::chrono::duration<double>(Clock::now() - began).count();

    std::sort(outcomes.begin(), outcomes.end(), [](const Outcome& a, const Outcome& b) {
        return std::tie(a.part, a.edit, a.k) < std::tie(b.part, b.edit, b.k);
    });
    if (const char* file = std::getenv("RAY_DIRECTEDIT_RESULTS")) {
        std::ofstream out(file);
        for (const Outcome& o : outcomes)
            out << o.part << '\t' << o.kind << '\t' << o.edit << '\t' << o.k << '\t' << o.note << '\t' << o.result
                << '\t' << static_cast<long long>(o.ms) << '\t' << clean(o.message) << '\n';
    }

    // The acceptance lines of PLAN.md T-080.
    struct Line {
        const char* name;
        double required;
        bool realOnly;
        std::vector<std::string> edits;
        std::string note; // empty: any
    };
    const std::vector<Line> lines = {
        {"delete plain hole", 0.95, false, {"del_hole"}, "plain"},
        {"delete hole with its counterbore, countersink or chamfer",
         0.95,
         false,
         {"del_hole_stack", "del_hole"},
         "in_stack|"},
        {"delete closed pocket", 0.95, false, {"del_pocket"}, ""},
        {"resize hole larger", 0.97, false, {"rs_grow"}, ""},
        {"resize hole smaller", 0.95, false, {"rs_shrink"}, ""},
        {"push planar face out", 0.95, false, {"pp_out"}, ""},
        {"pull planar face in", 0.95, false, {"pp_in"}, ""},
        {"delete fillet touching no other blend (real parts)", 0.75, true, {"del_fillet"}, "isolated"},
        {"delete whole blend run (real parts)", 0.50, true, {"del_fillet_chain"}, ""},
    };
    const auto inLine = [](const Line& line, const Outcome& o) {
        if (line.realOnly && o.kind != "real")
            return false;
        if (std::find(line.edits.begin(), line.edits.end(), o.edit) == line.edits.end())
            return false;
        if (line.note.empty())
            return true;
        if (line.note == "in_stack|") // the stack cases, and the bores picked out of a stack
            return o.edit == "del_hole_stack" || o.note == "in_stack";
        return o.note == line.note;
    };
    std::printf("\nDirect edits on the S9 set: %zu of %zu parts, %zu cases, %.1f s (%.1f s with abandoned edits)\n",
                parts.size(), listed.size(), outcomes.size(), seconds, withAbandoned);
    for (const std::string& m : missing)
        std::printf("  NOT PRESENT (rates are without it): %s\n", m.c_str());
    for (const std::string& f : loadFailures)
        std::printf("  NOT LOADED: %s\n", f.c_str());
    std::vector<std::pair<const Line*, Tally>> tallies;
    for (const Line& line : lines) {
        Tally t;
        for (const Outcome& o : outcomes)
            if (inLine(line, o)) {
                ++t.total;
                t.ok += o.result == "ok" ? 1 : 0;
            }
        tallies.emplace_back(&line, t);
        std::printf("  %-58s %4d of %4d  %5.1f%%  (at least %.0f%%)\n", line.name, t.ok, t.total, 100.0 * t.rate(),
                    100.0 * line.required);
    }
    // All edits, real and synthetic, for the record.
    std::map<std::string, std::map<std::string, Tally>> byEdit;
    for (const Outcome& o : outcomes) {
        Tally& t = byEdit[o.edit][o.kind + (o.note.empty() ? "" : " " + o.note)];
        ++t.total;
        t.ok += o.result == "ok" ? 1 : 0;
    }
    for (const auto& [edit, rows] : byEdit)
        for (const auto& [group, t] : rows)
            std::printf("    %-18s %-22s %4d of %4d\n", edit.c_str(), group.c_str(), t.ok, t.total);
    std::map<std::string, std::pair<double, double>> times; // edit -> (max ms of a finished call, max cancel ms)
    int failures = 0;
    for (const Outcome& o : outcomes) {
        auto& [slowest, cancel] = times[o.edit];
        if (o.result != "timeout")
            slowest = std::max(slowest, o.ms);
        else
            cancel = std::max(cancel, o.cancelMs);
        if (o.result != "ok") {
            ++failures;
            std::printf("  FAIL %-14s %-17s k%d %-12s %-10s %7.0f ms  %s\n", o.part.c_str(), o.edit.c_str(), o.k,
                        o.note.c_str(), o.result.c_str(), o.ms, o.message.c_str());
        }
    }
    for (const auto& [edit, t] : times)
        std::printf("  time %-18s slowest finished call %8.0f ms, slowest return after a timeout %6.0f ms\n",
                    edit.c_str(), t.first, t.second);

    if (!only.empty())
        return; // one case for profiling: nothing to judge
    // Rates.
    for (const auto& [line, t] : tallies) {
        INFO(line->name << ": " << t.ok << " of " << t.total);
        CHECK(t.total > 0);
        CHECK(t.rate() >= line->required);
    }
    // Every success is a valid closed solid that changed the expected way; every failure is a KernelFailure with a
    // message, never an exception's text; a call stopped for its time limit hands control back within 100 ms.
    for (const Outcome& o : outcomes) {
        INFO(o.part << " " << o.edit << " k" << o.k << ": " << o.result << " " << o.message);
        CHECK(o.result != "bad_invalid");
        CHECK(o.result != "bad_no_op");
        CHECK(o.message.find("wrong direction") == std::string::npos);
        CHECK_FALSE(o.wrongCode);
        CHECK(o.historyFault.empty());
        if (o.result == "refused") {
            CHECK_FALSE(o.message.empty());
            CHECK(o.message.rfind("The geometry kernel failed:", 0) == std::string::npos);
        }
        if (o.result == "timeout")
            CHECK(o.cancelMs < 100.0);
    }
    // An edit abandoned for its time limit stops within 10 s: none is still running 10 s after the last case returned.
    INFO("abandoned edits ran on for " << withAbandoned - seconds << " s after the last case");
    CHECK(withAbandoned - seconds < 10.0);
    CHECK(testing::structuredExceptions() == 0);
}
