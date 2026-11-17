// S5 spike: standalone driver for FreeCAD Adaptive2d. Compiled twice:
//   S5_VARIANT_MAIN -> FreeCAD main branch Adaptive.cpp (Clipper2 API)
//   otherwise       -> FreeCAD 1.1.4 Adaptive.cpp (bundled Clipper 6.4.2)
#include <functional>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>
#include "Adaptive.hpp"
#include "cases.h"

using namespace AdaptivePath;
using Clock = std::chrono::steady_clock;

struct NullBuf : std::streambuf {
    int overflow(int c) override { return c; }
    std::streamsize xsputn(const char*, std::streamsize n) override { return n; }
};

struct RunResult {
    std::list<AdaptiveOutput> out;
    double ms = 0;
    int callbacks = 0;
    double firstCallbackMs = -1, cancelReturnMs = -1;
};

static double gTolerance = 0.1;

static RunResult runCase(const s5::Case& c, double stepover, bool cancelAtFirstCallback = false) {
    Adaptive2d a;
    a.toolDiameter = s5::kToolDia;
    a.stepOverFactor = stepover;
    a.tolerance = gTolerance;  // 0.1 is the FreeCAD default
    a.stockToLeave = 0;
    a.forceInsideOut = false;  // FreeCAD default; needed for the open pocket
    a.finishingProfile = true;
    a.keepToolDownDistRatio = 3.0;
    a.opType = OperationType::otClearingInside;
#ifdef S5_VARIANT_MAIN
    a.helixRampTargetDiameter = 0.75 * s5::kToolDia;
    a.helixRampMinDiameter = 0;
#else
    a.helixRampDiameter = 0.75 * s5::kToolDia;
#endif
    RunResult r;
    auto t0 = Clock::now();
    auto ms = [&] { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); };
    std::function<bool(TPaths)> cb = [&](TPaths) {
        r.callbacks++;
        if (r.firstCallbackMs < 0) r.firstCallbackMs = ms();
        return cancelAtFirstCallback;
    };
#ifdef S5_VARIANT_MAIN
    r.out = a.Execute(c.stock, c.pocket, DPaths{}, cb);
#else
    r.out = a.Execute(c.stock, c.pocket, cb);
#endif
    r.ms = ms();
    if (cancelAtFirstCallback && r.firstCallbackMs >= 0) r.cancelReturnMs = r.ms - r.firstCallbackMs;
    return r;
}

struct Fnv {
    std::uint64_t h = 1469598103934665603ull;
    void add(const void* p, size_t n) {
        const unsigned char* b = (const unsigned char*)p;
        for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    }
    void d(double v) { add(&v, sizeof v); }
    void i(std::int64_t v) { add(&v, sizeof v); }
};
static std::uint64_t hashOut(const std::list<AdaptiveOutput>& out, size_t* npts = nullptr) {
    Fnv f;
    size_t n = 0;
    f.i((std::int64_t)out.size());
    for (auto& o : out) {
        f.d(o.HelixCenterPoint.first); f.d(o.HelixCenterPoint.second);
        f.d(o.StartPoint.first); f.d(o.StartPoint.second);
        f.i(o.ReturnMotionType);
        f.i((std::int64_t)o.AdaptivePaths.size());
        for (auto& p : o.AdaptivePaths) {
            f.i(p.first); f.i((std::int64_t)p.second.size());
            for (auto& q : p.second) { f.d(q.first); f.d(q.second); n++; }
        }
    }
    if (npts) *npts = n;
    return f.h;
}
static void writeOut(const std::list<AdaptiveOutput>& out, const char* path) {
    FILE* fp = fopen(path, "wb");
    if (!fp) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    fprintf(fp, "S5TP 1\nregions %zu\n", out.size());
    for (auto& o : out) {
        fprintf(fp, "region %.17g %.17g %.17g %.17g %d %zu\n", o.HelixCenterPoint.first, o.HelixCenterPoint.second,
                o.StartPoint.first, o.StartPoint.second, o.ReturnMotionType, o.AdaptivePaths.size());
#ifdef S5_VARIANT_MAIN
        fprintf(fp, "flags %d %d %d %d %d %d %d\n", o.StartPointNotFound, o.LeadPathFailed, o.UnexpectedRotateIterations,
                o.TooManyFailedEngagements, o.UnclearedAreaRemains, o.FailedToSetUpFinishingPass, o.FinishingLeadInFailed);
#else
        fprintf(fp, "flags 0 0 0 0 0 0 0\n");
#endif
        for (auto& p : o.AdaptivePaths) {
            fprintf(fp, "path %d %zu\n", p.first, p.second.size());
            for (auto& q : p.second) fprintf(fp, "%.17g %.17g\n", q.first, q.second);
        }
    }
    fclose(fp);
}

int main(int argc, char** argv) {
    static NullBuf nb;
    std::cout.rdbuf(&nb);  // Adaptive prints to cout
    auto cases = s5::makeCases();
    auto find = [&](const std::string& id) -> const s5::Case& {
        for (auto& c : cases) if (c.id == id) return c;
        fprintf(stderr, "unknown case %s\n", id.c_str()); exit(2);
    };
    std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "run" && argc >= 5) {  // run <case> <stepover> <outfile> [timing reps] [tolerance]
        const s5::Case& c = find(argv[2]);
        double so = atof(argv[3]);
        int reps = argc > 5 ? atoi(argv[5]) : 1;
        if (argc > 6) gTolerance = atof(argv[6]);
        std::vector<double> t;
        RunResult r;
        for (int i = 0; i < reps; i++) { r = runCase(c, so); t.push_back(r.ms); }
        std::sort(t.begin(), t.end());
        size_t n; std::uint64_t h = hashOut(r.out, &n);
        writeOut(r.out, argv[4]);
        printf("case=%s stepover=%g regions=%zu points=%zu hash=%016llx ms_median=%.1f ms_min=%.1f ms_max=%.1f callbacks=%d\n",
               c.id.c_str(), so, r.out.size(), n, (unsigned long long)h, t[t.size() / 2], t.front(), t.back(), r.callbacks);
        return 0;
    }
    if (mode == "det" && argc >= 4) {  // det <stepover> <reps>: repeated runs in one process
        double so = atof(argv[2]); int reps = atoi(argv[3]); int bad = 0;
        for (auto& c : cases) {
            std::vector<std::uint64_t> hs;
            for (int i = 0; i < reps; i++) hs.push_back(hashOut(runCase(c, so).out));
            int diff = 0; for (auto h : hs) diff += (h != hs[0]);
            std::uint64_t first = hs[0];
            std::sort(hs.begin(), hs.end());
            size_t distinct = std::unique(hs.begin(), hs.end()) - hs.begin();
            printf("det case=%s reps=%d first=%016llx differing_from_first=%d distinct=%zu\n", c.id.c_str(), reps,
                   (unsigned long long)first, diff, distinct);
            fflush(stdout);
            bad += diff;
        }
        return bad ? 1 : 0;
    }
    if (mode == "mt" && argc >= 5) {  // mt <stepover> <threads> <reps>: concurrent independent instances
        double so = atof(argv[2]); int nt = atoi(argv[3]); int reps = atoi(argv[4]);
        std::vector<std::vector<std::uint64_t>> hs(nt);
        std::vector<std::thread> th;
        for (int t = 0; t < nt; t++)
            th.emplace_back([&, t] {
                for (int i = 0; i < reps; i++) for (auto& c : cases) {
                    fprintf(stderr, "start t=%d rep=%d %s%c", t, i, c.id.c_str(), 10);
                    hs[t].push_back(hashOut(runCase(c, so).out));
                    fprintf(stderr, "end t=%d rep=%d %s%c", t, i, c.id.c_str(), 10);
                }
            });
        for (auto& t : th) t.join();
        int bad = 0;
        for (size_t k = 0; k < cases.size(); k++) {
            std::vector<std::uint64_t> all;
            for (int t = 0; t < nt; t++) for (int i = 0; i < reps; i++) all.push_back(hs[t][i * cases.size() + k]);
            std::uint64_t first = all[0];
            std::sort(all.begin(), all.end());
            size_t distinct = std::unique(all.begin(), all.end()) - all.begin();
            printf("mt case=%s threads=%d reps=%d first=%016llx distinct=%zu\n", cases[k].id.c_str(), nt, reps,
                   (unsigned long long)first, distinct);
            bad += distinct != 1;
        }
        return bad ? 1 : 0;
    }
    if (mode == "cancel" && argc >= 3) {  // cancel <stepover>
        double so = atof(argv[2]);
        for (auto& c : cases) {
            RunResult r = runCase(c, so, true);
            printf("cancel case=%s first_callback_ms=%.1f return_after_cancel_ms=%.1f total_ms=%.1f\n", c.id.c_str(),
                   r.firstCallbackMs, r.cancelReturnMs, r.ms);
        }
        return 0;
    }
    fprintf(stderr, "usage: run <case> <stepover> <out> [reps] | det <stepover> <reps> | mt <stepover> <threads> <reps> | cancel <stepover>\n");
    return 2;
}
