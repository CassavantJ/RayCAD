// The rebuilder: caching, incremental rebuild, failures, cancellation and determinism (task T-010).
#include "rebuild_test_util.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using ray::ErrorCode;
using ray::JobContext;
using ray::doc::BuildResultPtr;
using ray::doc::DocState;
using ray::doc::DocStatePtr;
using ray::doc::Feature;
using ray::doc::FeatureStatus;
using ray::doc::test::bodyId;
using ray::doc::test::BuildCounter;
using ray::doc::test::featureId;
using ray::doc::test::RebuildEnv;
using ray::doc::test::reportOf;
using ray::doc::test::stepFeature;

namespace {

DocStatePtr stateOf(std::vector<Feature> features)
{
    auto state = std::make_shared<DocState>();
    state->features = std::move(features);
    return state;
}

int builds(std::uint64_t n)
{
    return BuildCounter::instance().count(featureId(n));
}

// A minimal feature type with a name chosen by the test.
class NamedType final : public ray::doc::IFeatureType {
public:
    explicit NamedType(std::string name) : name_(std::move(name)) {}
    std::string_view typeName() const override { return name_; }
    std::string_view propsSchemaJson() const override { return "{}"; }
    std::vector<ray::naming::TopoRef> references(const Feature&) const override { return {}; }
    ray::Result<ray::doc::FeatureOutput> rebuild(const Feature&, ray::doc::RebuildContext&) const override
    {
        return ray::doc::FeatureOutput{};
    }

private:
    std::string name_;
};

} // namespace

TEST_CASE("FeatureRegistry keeps the first type of a name and ignores null and unnamed types", "[doc][rebuild]")
{
    ray::doc::FeatureRegistry registry;
    registry.add(nullptr);
    registry.add(std::make_unique<NamedType>(""));
    registry.add(std::make_unique<NamedType>("b"));
    auto second = std::make_unique<NamedType>("b");
    const ray::doc::IFeatureType* firstB = registry.find("b");
    registry.add(std::move(second));
    registry.add(std::make_unique<NamedType>("a"));
    CHECK(registry.typeNames() == std::vector<std::string>{"a", "b"});
    CHECK(registry.find("b") == firstB);
    CHECK(registry.find("c") == nullptr);

    REQUIRE(ray::doc::registerBuiltinFeatureTypes(registry));
    CHECK(registry.find("import") != nullptr);
    CHECK(registry.find("import")->references(Feature{}).empty());
    CHECK_FALSE(registry.find("import")->propsSchemaJson().empty());
    const auto again = ray::doc::registerBuiltinFeatureTypes(registry);
    REQUIRE_FALSE(again);
    CHECK(again.error().code == ErrorCode::InvalidArgument);

    const auto builtin = ray::doc::builtinFeatureRegistry();
    REQUIRE(builtin);
    CHECK(builtin->typeNames() == std::vector<std::string>{"chamfer", "datum_plane", "extrude", "fillet", "hole",
                                                           "import", "revolve", "sketch"});
}

TEST_CASE("an empty document rebuilds to an empty result; a null state is rejected", "[doc][rebuild]")
{
    RebuildEnv env;
    const BuildResultPtr build = env.rebuild(std::make_shared<DocState>());
    CHECK(build->bodies.empty());
    CHECK(build->reports.empty());
    JobContext ctx;
    CHECK(env.rebuilder->rebuild(nullptr, ctx).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("unchanged features report cacheHit and are not built again", "[doc][rebuild]")
{
    RebuildEnv env;
    const DocStatePtr state =
        stateOf({stepFeature(101, 'a', "new", 10), stepFeature(102, 'a', "grow", 5), stepFeature(103, 'b', "new", 7)});
    const BuildResultPtr first = env.rebuild(state);
    REQUIRE(first->reports.size() == 3);
    for (const auto& r : first->reports) {
        CHECK(r.status == FeatureStatus::Ok);
        CHECK_FALSE(r.cacheHit);
    }
    REQUIRE(first->bodies.size() == 2);
    CHECK(ray::kernel::bounds(first->bodies.at(bodyId('a')).shape).max.x == 15.0);
    const int counts[] = {builds(101), builds(102), builds(103)};

    // The same snapshot, and an equal copy of it, hit the cache for every feature.
    for (const DocStatePtr& again : {state, DocStatePtr(std::make_shared<DocState>(*state))}) {
        const BuildResultPtr second = env.rebuild(again);
        REQUIRE(second->reports.size() == 3);
        for (const auto& r : second->reports) {
            CHECK(r.status == FeatureStatus::Ok);
            CHECK(r.cacheHit);
            CHECK(r.seconds == 0.0);
        }
        CHECK(builds(101) == counts[0]);
        CHECK(builds(102) == counts[1]);
        CHECK(builds(103) == counts[2]);
        for (const auto& [id, body] : first->bodies) {
            CHECK(second->bodies.at(id).shape.contentHash() == body.shape.contentHash());
            CHECK(second->bodies.at(id).map.serialize() == body.map.serialize());
        }
    }

    // A rename changes no geometry: still all hits.
    auto renamed = std::make_shared<DocState>(*state);
    renamed->features[1].name = "Renamed";
    // Held in a variable: a range-for over env.rebuild(...)->reports would read the reports after the temporary
    // result (the only owner) was released.
    const auto renamedResult = env.rebuild(renamed);
    for (const auto& r : renamedResult->reports) {
        CHECK(r.cacheHit);
    }
}

TEST_CASE("editing feature N recomputes only N and the later features of the same body", "[doc][rebuild]")
{
    RebuildEnv env;
    std::vector<Feature> features = {stepFeature(201, 'a', "new", 10), stepFeature(202, 'b', "new", 10),
                                     stepFeature(203, 'a', "grow", 1), stepFeature(204, 'b', "grow", 1),
                                     stepFeature(205, 'a', "grow", 1), stepFeature(206, 'b', "grow", 1)};
    const DocStatePtr original = stateOf(features);
    const BuildResultPtr base = env.rebuild(original);
    auto snapshot = [] {
        std::vector<int> v;
        for (std::uint64_t n = 201; n <= 206; ++n) {
            v.push_back(builds(n));
        }
        return v;
    };

    SECTION("edit a middle feature of body a")
    {
        const std::vector<int> before = snapshot();
        features[2] = stepFeature(203, 'a', "grow", 2);
        const BuildResultPtr build = env.rebuild(stateOf(features));
        const std::vector<int> after = snapshot();
        // 203 and 205 (body a, at and after the edit) ran again; everything else came from the cache.
        CHECK(after == std::vector<int>{before[0], before[1], before[2] + 1, before[3], before[4] + 1, before[5]});
        CHECK(reportOf(*build, featureId(201)).cacheHit);
        CHECK(reportOf(*build, featureId(202)).cacheHit);
        CHECK_FALSE(reportOf(*build, featureId(203)).cacheHit);
        CHECK(reportOf(*build, featureId(204)).cacheHit);
        CHECK_FALSE(reportOf(*build, featureId(205)).cacheHit);
        CHECK(reportOf(*build, featureId(206)).cacheHit);
        CHECK(ray::kernel::bounds(build->bodies.at(bodyId('a')).shape).max.x == 13.0);
        CHECK(build->bodies.at(bodyId('b')).shape.contentHash() == base->bodies.at(bodyId('b')).shape.contentHash());

        // Going back to the original (undo) builds nothing at all.
        const std::vector<int> edited = snapshot();
        const BuildResultPtr back = env.rebuild(original);
        CHECK(snapshot() == edited);
        for (const auto& r : back->reports) {
            CHECK(r.cacheHit);
        }
        CHECK(back->bodies.at(bodyId('a')).shape.contentHash() == base->bodies.at(bodyId('a')).shape.contentHash());
    }

    SECTION("edit the first feature of body b")
    {
        const std::vector<int> before = snapshot();
        features[1] = stepFeature(202, 'b', "new", 20);
        env.rebuild(stateOf(features));
        CHECK(snapshot() ==
              std::vector<int>{before[0], before[1] + 1, before[2], before[3] + 1, before[4], before[5] + 1});
    }

    SECTION("an edit that gives the same body output stops the recomputation there")
    {
        // Reordering two independent features changes no body's inputs: nothing is rebuilt.
        const std::vector<int> before = snapshot();
        std::swap(features[0], features[1]);
        env.rebuild(stateOf(features));
        CHECK(snapshot() == before);
    }
}

TEST_CASE("a failed feature marks later features of its body NotBuilt and keeps earlier output", "[doc][rebuild]")
{
    RebuildEnv env;
    const BuildResultPtr goodA = env.rebuild(stateOf({stepFeature(301, 'a', "new", 10)}));
    std::vector<Feature> features = {stepFeature(301, 'a', "new", 10), stepFeature(302, 'a', "grow", 5),
                                     stepFeature(303, 'a', "grow", 5), stepFeature(304, 'b', "new", 4),
                                     stepFeature(305, 'b', "grow", 4), stepFeature(306, 'c', "grow", 1)};
    // The whole history builds once (feature 306 fails: body c does not exist).
    const BuildResultPtr good = env.rebuild(stateOf(features));
    CHECK(reportOf(*good, featureId(303)).status == FeatureStatus::Ok);
    CHECK(reportOf(*good, featureId(306)).status == FeatureStatus::Failed);

    // Feature 302 is edited so that it fails (before it even reads its body).
    features[1] = stepFeature(302, 'a', "grow", 5, R"(,"fail":true)");
    const BuildResultPtr build = env.rebuild(stateOf(features));
    REQUIRE(build->reports.size() == 6);
    CHECK(reportOf(*build, featureId(301)).status == FeatureStatus::Ok);
    const auto& failed = reportOf(*build, featureId(302));
    CHECK(failed.status == FeatureStatus::Failed);
    CHECK_THAT(failed.message, ContainsSubstring("Test failure in Step 302"));
    CHECK(reportOf(*build, featureId(303)).status == FeatureStatus::NotBuilt);
    CHECK_THAT(reportOf(*build, featureId(303)).message, ContainsSubstring("earlier failed feature"));
    // Body b does not depend on body a: its features are not affected (and come from the cache).
    CHECK(reportOf(*build, featureId(304)).status == FeatureStatus::Ok);
    CHECK(reportOf(*build, featureId(304)).cacheHit);
    CHECK(reportOf(*build, featureId(305)).status == FeatureStatus::Ok);
    CHECK(reportOf(*build, featureId(305)).cacheHit);
    // Body c was never made; after a failure that may be the failed feature's fault, so: NotBuilt, not Failed.
    CHECK(reportOf(*build, featureId(306)).status == FeatureStatus::NotBuilt);

    // Body a is what feature 301 made: earlier output is intact.
    REQUIRE(build->bodies.count(bodyId('a')) == 1);
    CHECK(build->bodies.at(bodyId('a')).shape.contentHash() == goodA->bodies.at(bodyId('a')).shape.contentHash());
    CHECK(build->bodies.at(bodyId('a')).map.serialize() == goodA->bodies.at(bodyId('a')).map.serialize());
    CHECK(ray::kernel::bounds(build->bodies.at(bodyId('b')).shape).max.x == 8.0);
    CHECK(build->bodies.count(bodyId('c')) == 0);

    // Failures are not cached: building again reports the same, and fixing the feature builds the chain.
    const int failedBuilds = builds(302);
    CHECK(reportOf(*env.rebuild(stateOf(features)), featureId(302)).status == FeatureStatus::Failed);
    CHECK(builds(302) == failedBuilds + 1);
    features[1] = stepFeature(302, 'a', "grow", 5);
    const BuildResultPtr repaired = env.rebuild(stateOf(features));
    CHECK(reportOf(*repaired, featureId(301)).cacheHit);
    CHECK(reportOf(*repaired, featureId(302)).status == FeatureStatus::Ok);
    CHECK(reportOf(*repaired, featureId(303)).status == FeatureStatus::Ok);
    CHECK(reportOf(*repaired, featureId(304)).cacheHit);
    CHECK(ray::kernel::bounds(repaired->bodies.at(bodyId('a')).shape).max.x == 20.0);
}

TEST_CASE("a new feature that fails before it ever built stops every body that existed before it", "[doc][rebuild]")
{
    // Nothing tells the rebuilder which bodies the new feature was meant to change, so it assumes all of them; a
    // body written again after the failure builds on.
    RebuildEnv env;
    const BuildResultPtr build = env.rebuild(
        stateOf({stepFeature(351, 'a', "new", 10), stepFeature(352, 'b', "new", 10),
                 stepFeature(353, 'a', "grow", 1, R"(,"fail":true)"), stepFeature(354, 'a', "grow", 1),
                 stepFeature(355, 'b', "grow", 1), stepFeature(356, 'd', "new", 2), stepFeature(357, 'd', "grow", 2)}));
    CHECK(reportOf(*build, featureId(353)).status == FeatureStatus::Failed);
    CHECK(reportOf(*build, featureId(354)).status == FeatureStatus::NotBuilt);
    CHECK(reportOf(*build, featureId(355)).status == FeatureStatus::NotBuilt);
    CHECK(reportOf(*build, featureId(356)).status == FeatureStatus::Ok);
    CHECK(reportOf(*build, featureId(357)).status == FeatureStatus::Ok);
    CHECK(ray::kernel::bounds(build->bodies.at(bodyId('a')).shape).max.x == 10.0);
    CHECK(ray::kernel::bounds(build->bodies.at(bodyId('b')).shape).max.x == 10.0);
    CHECK(ray::kernel::bounds(build->bodies.at(bodyId('d')).shape).max.x == 4.0);
}

TEST_CASE("without an earlier failure, reading a missing body fails the feature", "[doc][rebuild]")
{
    RebuildEnv env;
    const BuildResultPtr build = env.rebuild(stateOf({stepFeature(401, 'z', "grow", 1)}));
    CHECK(reportOf(*build, featureId(401)).status == FeatureStatus::Failed);
    CHECK_THAT(reportOf(*build, featureId(401)).message, ContainsSubstring("does not exist"));
}

TEST_CASE("unknown types, throwing features and suppressed features never throw out of rebuild", "[doc][rebuild]")
{
    RebuildEnv env;
    Feature unknown;
    unknown.id = featureId(501);
    unknown.type = "no.such.type";
    unknown.name = "Unknown";
    Feature suppressed = stepFeature(503, 'b', "new", 3);
    suppressed.suppressed = true;
    const BuildResultPtr build = env.rebuild(stateOf(
        {unknown, stepFeature(502, 'a', "new", 1, R"(,"throw":true)"), suppressed, stepFeature(504, 'd', "new", 2)}));
    CHECK(reportOf(*build, featureId(501)).status == FeatureStatus::Failed);
    CHECK_THAT(reportOf(*build, featureId(501)).message, ContainsSubstring("no.such.type"));
    CHECK(reportOf(*build, featureId(502)).status == FeatureStatus::Failed);
    CHECK_THAT(reportOf(*build, featureId(502)).message, ContainsSubstring("internal error"));
    CHECK(reportOf(*build, featureId(503)).status == FeatureStatus::Suppressed);
    CHECK(reportOf(*build, featureId(504)).status == FeatureStatus::Ok);
    CHECK(build->bodies.count(bodyId('b')) == 0);
    CHECK(build->bodies.count(bodyId('d')) == 1);
}

TEST_CASE("cancelling a rebuild returns within 1 s", "[doc][rebuild]")
{
    RebuildEnv env;
    const DocStatePtr state =
        stateOf({stepFeature(601, 'a', "new", 1), stepFeature(602, 'a', "grow", 1, R"(,"slowMs":20000)"),
                 stepFeature(603, 'a', "grow", 1)});
    auto rebuilder = env.rebuilder;
    auto job = env.jobs->submit<BuildResultPtr>(
        {"Rebuild"}, [rebuilder, state](JobContext& ctx) { return rebuilder->rebuild(state, ctx); });
    // Wait until the slow feature is running, then cancel.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (builds(602) == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(builds(602) == 1);
    const auto start = std::chrono::steady_clock::now();
    job.cancel();
    const auto result = job.wait();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::Cancelled);
    CHECK(elapsed < std::chrono::seconds(1));
    CHECK(builds(603) == 0);

    // The feature finished before the cancel is cached, so the next rebuild starts where this one stopped.
    ray::CancelSource source;
    source.cancel();
    JobContext cancelled{source.token(), {}};
    CHECK(env.rebuilder->rebuild(state, cancelled).error().code == ErrorCode::Cancelled);
    const auto quick = stateOf({stepFeature(601, 'a', "new", 1)});
    CHECK(reportOf(*env.rebuild(quick), featureId(601)).cacheHit);
}

TEST_CASE("the rebuild output is identical for 1 and 16 workers", "[doc][rebuild]")
{
    // An import with four bodies (read in parallel) and test features on two bodies.
    std::string reference;
    DocStatePtr state;
    std::shared_ptr<ray::doc::ObjectPool> pool = ray::doc::makeObjectPool();
    for (const int workers : {1, 16, 1}) {
        INFO("workers " << workers);
        std::string print;
        {
            ray::doc::test::DocEnv env(workers, state, pool);
            if (!state) {
                env.import(ray::doc::test::dataFile("assembly_mm.step"), 701);
                env.run(ray::doc::test::addStep(702, 'a', "new", 10));
                env.run(ray::doc::test::addStep(703, 'a', "grow", 3));
                env.run(ray::doc::test::addStep(704, 'b', "new", 4));
                state = env.doc->state();
            }
            REQUIRE(env.settle());
            REQUIRE(env.doc->builtState() == state);
            const BuildResultPtr build = env.doc->build();
            REQUIRE(build);
            CHECK(build->bodies.size() == 6);
            for (const auto& r : build->reports) {
                CHECK((r.status == FeatureStatus::Ok || r.status == FeatureStatus::Warning));
            }
            CHECK(env.jobs->workerCount() == workers);
            print = ray::doc::test::fingerprint(*state, *build);
        }
        if (reference.empty()) {
            reference = print;
        }
        CHECK(print == reference);
    }
}
