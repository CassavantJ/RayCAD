// The spike S4 part with its 47 edit scenarios (task T-081a): every reference recorded on the baseline build,
// resolved after each edit, judged by the oracle. Gate: zero wrong-element resolutions. Prints the per-scenario
// table of spike S4 REPORT.md section 4.2.
#include <catch2/catch_test_macros.hpp>

#include "harness.h"
#include "s4_part.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace ray;
using namespace ray::naming_oracle;

namespace {

struct ScenarioResult {
    Tally tally;
    std::vector<std::string> wrongs;
    std::vector<std::string> failed;
    int built = 0;
};

unsigned workerCount()
{
    return std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
}

} // namespace

TEST_CASE("S4 part: no reference resolves to a wrong element in the 47 edit scenarios", "[s4]")
{
    const s4::Params p0 = s4::baseParams();
    const Script s0 = s4::makeScript(p0);
    FeatRefs picks;
    const Builder base = buildScript(s0, Mode::Author, picks);
    const Owners ow0 = buildOwners(builtOnly(s0, base));
    REQUIRE(base.have());
    for (const std::string& f : base.failed())
        std::printf("baseline: FAILED %s\n", f.c_str());
    CHECK(base.failed().empty());
    auto samples0 = sampleShape(base.shape());
    REQUIRE(samples0.has_value());
    std::map<std::string, int> skipped;
    const std::vector<Ref> refs = recordReferences(base, ow0, **samples0, &skipped);
    long nPick = 0;
    for (const auto& [f, v] : picks)
        nPick += static_cast<long>(v.size());
    std::printf("S4 baseline: %d features built, %u faces, %u edges, %u vertices; %zu references, %ld feature picks "
                "(%d unverifiable), %zu lineage events\n",
                base.built(), base.shape().count(SubKind::Face), base.shape().count(SubKind::Edge),
                base.shape().count(SubKind::Vertex), refs.size(), nPick, base.unverifiablePicks(),
                base.lineageEvents());
    for (const auto& [why, n] : skipped)
        std::printf("  not a reference: %s: %d\n", why.c_str(), n);
    CHECK(refs.size() >= 600);

    std::vector<s4::Scenario> list = s4::scenarios();
    REQUIRE(list.size() == 47);
    // RAYCAD_NAMING_ONLY=<scenario id> runs one scenario (for diagnosis).
    if (const char* only = std::getenv("RAYCAD_NAMING_ONLY")) {
        list.erase(std::remove_if(list.begin(), list.end(), [&](const s4::Scenario& s) { return s.id != only; }),
                   list.end());
        REQUIRE(!list.empty());
    }
    std::vector<ScenarioResult> results(list.size());
    std::atomic<std::size_t> next{0};
    auto worker = [&] {
        for (std::size_t i = next++; i < list.size(); i = next++) {
            const s4::Scenario& sc = list[i];
            s4::Params p = s4::baseParams();
            if (sc.p)
                sc.p(p);
            Script s = s4::makeScript(p);
            if (sc.s)
                sc.s(s);
            const Builder b = buildScript(s, Mode::Replay, picks);
            const Owners ow = buildOwners(builtOnly(s, b));
            ScenarioResult& out = results[i];
            out.failed = b.failed();
            out.built = b.built();
            auto smp = sampleShape(b.shape());
            if (!smp) {
                out.failed.push_back("sampling failed: " + smp.error().message);
                continue;
            }
            judgeAll(refs, b, ow, **smp, **samples0, out.tally, out.wrongs, 40, sc.id);
        }
    };
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < workerCount(); ++t)
        threads.emplace_back(worker);
    for (std::thread& t : threads)
        t.join();

    Tally total, plan;
    std::printf(
        "\n| scenario | built/failed | refs | Exact %% | Split %% | Merged %% | Repaired %% | Broken %% | gone / "
        "missed | exists: E+S+M %% | WRONG | conflicts |\n|-|-|-|-|-|-|-|-|-|-|-|-|\n");
    for (std::size_t i = 0; i < list.size(); ++i) {
        const Tally& t = results[i].tally;
        total.merge(t);
        if (s4::inPlanSet(list[i].id))
            plan.merge(t);
        std::printf("| %s | %d/%zu | %ld | %s | %s | %s | %s | %s | %ld / %ld | %s | %ld | %ld |\n", list[i].id.c_str(),
                    results[i].built, results[i].failed.size(), t.n, percent(t.state[0], t.n).c_str(),
                    percent(t.state[1], t.n).c_str(), percent(t.state[2], t.n).c_str(),
                    percent(t.state[3], t.n).c_str(), percent(t.state[4], t.n).c_str(),
                    t.verdict[static_cast<std::size_t>(Verdict::BrokenGone)],
                    t.verdict[static_cast<std::size_t>(Verdict::Missed)], percent(t.existingOk, t.existing).c_str(),
                    t.wrong(), t.conflicts);
    }
    for (std::size_t i = 0; i < list.size(); ++i) {
        for (const std::string& f : results[i].failed)
            std::printf("  %s: feature failed: %s\n", list[i].id.c_str(), f.c_str());
        for (const std::string& w : results[i].wrongs)
            std::printf("  %s\n", w.c_str());
    }
    std::printf("\nS4 total: %ld resolutions: Exact %s%%, Split %s%%, Merged %s%%, Repaired %s%%, Broken %s%% (really "
                "gone %ld, missed %ld); WRONG %ld; oracle conflicts %ld; mutants caught %ld of %ld; accepted by the "
                "new-territory rule %ld; not judged (no mesh) %ld\n",
                total.n, percent(total.state[0], total.n).c_str(), percent(total.state[1], total.n).c_str(),
                percent(total.state[2], total.n).c_str(), percent(total.state[3], total.n).c_str(),
                percent(total.state[4], total.n).c_str(), total.verdict[static_cast<std::size_t>(Verdict::BrokenGone)],
                total.verdict[static_cast<std::size_t>(Verdict::Missed)], total.wrong(), total.conflicts,
                total.mutantsCaught, total.mutants, total.newTerritory, total.unjudged);
    std::printf("PLAN 3.3 set: %ld resolutions, Exact %s%%, WRONG %ld\n", plan.n,
                percent(plan.state[0], plan.n).c_str(), plan.wrong());
    CHECK(total.mutantsCaught == total.mutants);
    CHECK(total.repairedUsable == 0);
    // The no-op rebuild resolves everything Exact (determinism).
    if (list.front().id == "N00")
        CHECK(results[0].tally.state[0] == results[0].tally.n);
    REQUIRE(total.wrong() == 0);
}
