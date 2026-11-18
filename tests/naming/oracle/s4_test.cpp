// The spike S4 part with its 47 edit scenarios (task T-081a): every reference recorded on the baseline build,
// resolved after each edit, judged by the oracle. Gates: zero wrong-element resolutions, none not judged, every
// new-territory acceptance and congruence conflict reviewed, no injectivity violation, every mutant caught.
// Prints the per-scenario table of spike S4 REPORT.md section 4.2 and every bucket (the log is the record).
#include <catch2/catch_test_macros.hpp>

#include "harness.h"
#include "s4_part.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

using namespace ray;
using namespace ray::naming_oracle;

namespace {

struct ScenarioResult {
    Tally tally;
    JudgeLog log;
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
    const auto ow0 = std::make_shared<const Owners>(buildOwners(builtOnly(s0, base)));
    REQUIRE(base.have());
    for (const std::string& f : base.failed())
        std::printf("baseline: FAILED %s\n", f.c_str());
    CHECK(base.failed().empty());
    auto samples0 = sampleShape(base.shape());
    REQUIRE(samples0.has_value());
    std::map<std::string, int> skipped;
    const std::vector<Ref> refs = recordReferences(base, ow0, *samples0, &skipped);
    std::printf("oracle: %s; baseline: %u faces without triangles\n", strictnessName().c_str(),
                (*samples0)->unmeshedFaces);
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
            out.log.maxWrongs = 40;
            judgeAll(refs, b, ow, **smp, out.tally, out.log, "S4 " + sc.id);
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
        "missed | exists: E+S+M %% (bar 95) | WRONG | not judged | conflicts | accepted |\n"
        "|-|-|-|-|-|-|-|-|-|-|-|-|-|-|\n");
    for (std::size_t i = 0; i < list.size(); ++i) {
        const Tally& t = results[i].tally;
        total.merge(t);
        if (s4::inPlanSet(list[i].id))
            plan.merge(t);
        const bool below = t.existing > 0 && 100 * t.existingOk < 95 * t.existing;
        std::printf("| %s | %d/%zu | %ld | %s | %s | %s | %s | %s | %ld / %ld | %s%s | %ld | %ld | %ld | %ld |\n",
                    list[i].id.c_str(), results[i].built, results[i].failed.size(), t.n,
                    percent(t.state[0], t.n).c_str(), percent(t.state[1], t.n).c_str(),
                    percent(t.state[2], t.n).c_str(), percent(t.state[3], t.n).c_str(),
                    percent(t.state[4], t.n).c_str(), t.verdict[static_cast<std::size_t>(Verdict::BrokenGone)],
                    t.verdict[static_cast<std::size_t>(Verdict::Missed)], percent(t.existingOk, t.existing).c_str(),
                    below ? " BELOW" : "", t.wrong(), t.unjudged(), t.conflicts, t.accepted);
    }
    for (std::size_t i = 0; i < list.size(); ++i) {
        for (const std::string& f : results[i].failed)
            std::printf("  %s: feature failed: %s\n", list[i].id.c_str(), f.c_str());
        for (const std::string& w : results[i].log.wrongs)
            std::printf("  %s\n", w.c_str());
    }
    for (std::size_t i = 0; i < list.size(); ++i)
        for (const std::string& n : results[i].log.notes)
            std::printf("  %s\n", n.c_str());
    // RAYCAD_NAMING_VERDICTS=<file>: one line per judged reference (to compare oracle settings).
    if (const char* trace = std::getenv("RAYCAD_NAMING_VERDICTS"))
        if (std::FILE* f = std::fopen(trace, "w")) {
            for (const ScenarioResult& r : results)
                for (const std::string& line : r.log.trace)
                    std::fprintf(f, "%s\n", line.c_str());
            std::fclose(f);
        }
    printTally("S4 total", total);
    std::printf("PLAN 3.3 set: %ld resolutions, Exact %s%%, WRONG %ld\n", plan.n,
                percent(plan.state[0], plan.n).c_str(), plan.wrong());
    CHECK(total.mutantsCaught() == total.mutantsMade());
    CHECK(total.repairedUsable == 0);
    CHECK(total.selfCheckFailures == 0);
    CHECK(total.injectivity == 0);
    CHECK(total.unjudged() == 0);
    CHECK(total.accepted == total.acceptedReviewed);
    CHECK(total.conflicts == total.conflictsReviewed);
    // The no-op rebuild resolves everything Exact (determinism).
    if (list.front().id == "N00")
        CHECK(results[0].tally.state[0] == results[0].tally.n);
    REQUIRE(total.wrong() == 0);
}
