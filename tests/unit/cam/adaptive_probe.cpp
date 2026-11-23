// The adaptive engine's probe (task T-060): one engine run in a process of its own, for the determinism test across
// processes (adaptive_determinism_test.cpp starts it) and for measuring by hand.
//   ray_cam_adaptive_probe hash <case> <stepover>    the BLAKE3 of the output's golden text
//   ray_cam_adaptive_probe text <case> <stepover>    the golden text itself
//   ray_cam_adaptive_probe stats <case> <stepover> [repeats]   the engine's counters and best time
//   ray_cam_adaptive_probe paths <case> <stepover>   each path: role, motion, points, largest engagement estimate
//   ray_cam_adaptive_probe roles <case> <stepover>   the sweep checker's statistics of one path role at a time
//   ray_cam_adaptive_probe check <case> <stepover> [repeats]
//                                                    run time (best and worst of the repeats), the engine's counters
//                                                    and the sweep checker's report
// Cases are the S5 pockets (tests/tools/sweepcheck/s5_cases.h).
#include "adaptive_test_util.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace ray;
using namespace ray::cam::adaptive2d;
using namespace ray::cam::adaptive2d::test;

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::fprintf(stderr, "usage: ray_cam_adaptive_probe hash|text|check <case> <stepover> [repeats]\n");
        return 2;
    }
    const std::string mode = argv[1];
    const std::string id = argv[2];
    const double stepover = std::atof(argv[3]);
    const int repeats = argc > 4 ? std::max(1, std::atoi(argv[4])) : 1;
    const auto cases = sw::s5::makeCases();
    const sw::s5::Case& c = sw::s5::caseById(cases, id);
    if (c.id != id) {
        std::fprintf(stderr, "unknown case %s\n", id.c_str());
        return 2;
    }
    double best = 1e300, worst = 0;
    Result<Output> out = tl::unexpected(Error{ErrorCode::Internal, "not run", {}});
    for (int i = 0; i < repeats; i++) {
        JobContext ctx;
        auto t0 = std::chrono::steady_clock::now();
        Input in = inputFor(c, stepover);
        // For calibration by hand: the soft limit on the link search (engine.h Limits::linkSearchSteps).
        if (const char* v = std::getenv("RAY_ADAPTIVE_LINK_STEPS")) {
            in.limits.linkSearchSteps = std::atoll(v);
        }
        if (const char* v = std::getenv("RAY_ADAPTIVE_SIDE_STEPS")) {
            in.limits.linkSideSteps = std::atoll(v);
        }
        out = runEngine(in, ctx);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        best = std::min(best, ms);
        worst = std::max(worst, ms);
    }
    if (!out) {
        std::printf("error %d %s\n", int(out.error().code), out.error().message.c_str());
        return 1;
    }
    if (mode == "stats") {
        const Stats& st = out->stats;
        std::fprintf(stderr,
                     "ms %.0f steps %lld cutArea %lld passes %lld links %lld linkSteps %lld limitHits %lld side %lld "
                     "engage %lld\n",
                     best, (long long)st.steps, (long long)st.cutAreaCalls, (long long)st.passes,
                     (long long)st.linkSearches, (long long)st.linkSearchSteps, (long long)st.linkSearchLimitHits,
                     (long long)st.linkSideProbes, (long long)st.engageSearches);
        return 0;
    }
    if (mode == "hash") {
        std::printf("%s\n", hashText(toText(*out)).c_str());
        return 0;
    }
    if (mode == "text") {
        std::fwrite(toText(*out).data(), 1, toText(*out).size(), stdout);
        return 0;
    }
    if (mode == "paths") {
        for (const Region& r : out->regions) {
            std::printf("region entry %d centre (%.3f, %.3f) r %.3f\n", int(r.entry.kind), toMm(r.entry.centre.x),
                        toMm(r.entry.centre.y), toMm(r.entry.radius));
            for (std::size_t i = 0; i < r.paths.size(); i++) {
                const Path& p = r.paths[i];
                double mx = -1;
                for (double e : p.engagementDeg) {
                    mx = std::max(mx, e);
                }
                std::printf("%zu role %d motion %d points %zu max %.1f from (%.3f, %.3f) to (%.3f, %.3f)\n", i,
                            int(p.role), int(p.motion), p.points.size(), mx, toMm(p.points.front().x),
                            toMm(p.points.front().y), toMm(p.points.back().x), toMm(p.points.back().y));
            }
        }
        return 0;
    }
    if (mode == "roles") {
        // The checker's statistics of one role at a time (the others still cut, unmeasured).
        const char* names[] = {"pass", "lead-in", "lead-out", "link", "finish"};
        for (int role = 0; role < 5; role++) {
            sw::Input in = checkInputFor(c, *out, stepover);
            std::size_t k = 0;
            for (const Region& r : out->regions) {
                if (r.entry.kind == EntryMode::None) {
                    continue;
                }
                for (std::size_t i = 0; i < r.paths.size(); i++) {
                    in.regions[k].paths[i].measured = int(r.paths[i].role) == role;
                }
                k++;
            }
            sw::Report rep = sw::check(in);
            std::printf("%-8s %s\n", names[role], sw::summary(rep).c_str());
        }
        return 0;
    }
    const Stats& s = out->stats;
    std::size_t points = 0;
    for (const Region& r : out->regions) {
        for (const Path& p : r.paths) {
            points += p.points.size();
        }
    }
    std::printf(
        "case=%s stepover=%g ms_best=%.0f ms_worst=%.0f repeats=%d points=%zu regions=%zu warnings=%zu grid=%lld "
        "steps=%lld cutArea=%lld passes=%lld linkSearches=%lld linkSteps=%lld linkLimitHits=%lld "
        "sideProbes=%lld engageSearches=%lld failedInARow=%lld\n",
        id.c_str(), stepover, best, worst, repeats, points, out->regions.size(), out->warnings.size(),
        (long long)s.gridUnitsPerMm, (long long)s.steps, (long long)s.cutAreaCalls, (long long)s.passes,
        (long long)s.linkSearches, (long long)s.linkSearchSteps, (long long)s.linkSearchLimitHits,
        (long long)s.linkSideProbes, (long long)s.engageSearches, (long long)s.failedPassesInARow);
    for (const Warning& w : out->warnings) {
        std::printf("  warning: %s\n", w.message.c_str());
    }
    auto t0 = std::chrono::steady_clock::now();
    sw::Report rep = sw::check(checkInputFor(c, *out, stepover));
    double checkMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  target=%.2f %s check_ms=%.0f\n", angleForStepover(stepover), sw::summary(rep).c_str(), checkMs);
    return 0;
}
