// The document with a rebuilder: rebuild scheduling, build listeners on the dispatcher thread, previews with a
// preview build, cancellation (task T-010).
#include "rebuild_test_util.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using ray::ErrorCode;
using ray::doc::BuildResultPtr;
using ray::doc::DocStatePtr;
using ray::doc::FeatureStatus;
using ray::doc::test::addStep;
using ray::doc::test::bodyId;
using ray::doc::test::BuildCounter;
using ray::doc::test::DocEnv;
using ray::doc::test::featureId;
using ray::doc::test::reportOf;

namespace {

struct Call {
    std::thread::id thread;
    DocStatePtr state;
    BuildResultPtr build;
};

} // namespace

TEST_CASE("a document with a rebuilder builds its initial state", "[doc][rebuild][document]")
{
    DocEnv env;
    CHECK(env.doc->rebuildPending());
    REQUIRE(env.settle());
    REQUIRE(env.doc->build());
    CHECK(env.doc->builtState() == env.doc->state());
    CHECK(env.doc->build()->bodies.empty());
    CHECK_FALSE(env.doc->buildError());
    CHECK_FALSE(env.doc->dirty());
}

TEST_CASE("executing a command schedules a rebuild; listeners get the build on the dispatcher thread",
          "[doc][rebuild][document]")
{
    DocEnv env;
    REQUIRE(env.settle());
    std::mutex mutex;
    std::vector<Call> calls;
    env.doc->subscribe([&](DocStatePtr state, BuildResultPtr build) {
        std::lock_guard<std::mutex> lock(mutex);
        calls.push_back(Call{std::this_thread::get_id(), std::move(state), std::move(build)});
    });

    const DocStatePtr s1 = env.run(addStep(2001, 'a', "new", 5));
    CHECK(env.doc->rebuildPending());
    REQUIRE(env.settle());
    env.ui.runPending();
    REQUIRE(env.doc->builtState() == s1);
    const BuildResultPtr build = env.doc->build();
    REQUIRE(build);
    CHECK(build->bodies.count(bodyId('a')) == 1);
    CHECK(reportOf(*build, featureId(2001)).status == FeatureStatus::Ok);

    // One call for the change (with the build of that moment) and one for the published build, both on this thread,
    // which runs the dispatcher.
    std::lock_guard<std::mutex> lock(mutex);
    REQUIRE(calls.size() == 2);
    for (const Call& call : calls) {
        CHECK(call.thread == std::this_thread::get_id());
        CHECK(call.state == s1);
    }
    CHECK(calls[1].build == build);
}

TEST_CASE("a newer edit cancels the running rebuild and only the newest build is published", "[doc][rebuild][document]")
{
    DocEnv env;
    REQUIRE(env.settle());
    std::vector<DocStatePtr> published;
    env.doc->subscribe([&](DocStatePtr state, BuildResultPtr build) {
        if (build && build->reports.size() == state->features.size()) {
            published.push_back(state);
        }
    });
    env.run(addStep(2101, 'a', "new", 5, R"(,"slowMs":20000)"));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (BuildCounter::instance().count(featureId(2101)) == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(env.doc->undo()); // the slow rebuild is now pointless
    const DocStatePtr s2 = env.run(addStep(2102, 'b', "new", 5));
    REQUIRE(env.settle(std::chrono::milliseconds(5000)));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    env.ui.runPending();
    CHECK(env.doc->builtState() == s2);
    CHECK(env.doc->build()->bodies.count(bodyId('a')) == 0);
    CHECK(env.doc->build()->bodies.count(bodyId('b')) == 1);
    for (const DocStatePtr& state : published) {
        CHECK(state != nullptr);
        CHECK(state->features.size() <= 1);
    }
    REQUIRE_FALSE(published.empty());
    CHECK(published.back() == s2);
}

TEST_CASE("cancelRebuild stops a slow rebuild within 1 s and scheduleRebuild starts it again",
          "[doc][rebuild][document]")
{
    DocEnv env;
    REQUIRE(env.settle());
    const BuildResultPtr empty = env.doc->build();
    const DocStatePtr slow = env.run(addStep(2201, 'a', "new", 5, R"(,"slowMs":300)"));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (BuildCounter::instance().count(featureId(2201)) == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto start = std::chrono::steady_clock::now();
    env.doc->cancelRebuild();
    REQUIRE(env.settle(std::chrono::milliseconds(1000)));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
    REQUIRE(env.doc->buildError());
    CHECK(env.doc->buildError()->code == ErrorCode::Cancelled);
    CHECK(env.doc->build() == empty); // the last good build stays
    CHECK(env.doc->builtState() != slow);

    env.doc->scheduleRebuild();
    CHECK(env.doc->rebuildPending());
    REQUIRE(env.settle());
    CHECK_FALSE(env.doc->buildError());
    CHECK(env.doc->builtState() == slow);
    CHECK(env.doc->build()->bodies.count(bodyId('a')) == 1);
}

TEST_CASE("a preview gets a preview build and leaves the document build alone", "[doc][rebuild][document]")
{
    DocEnv env;
    env.run(addStep(2301, 'a', "new", 5));
    REQUIRE(env.settle());
    const BuildResultPtr build = env.doc->build();
    const DocStatePtr state = env.doc->state();
    std::vector<std::pair<DocStatePtr, BuildResultPtr>> previews;
    int changes = 0;
    env.doc->subscribe([&](DocStatePtr, BuildResultPtr) { ++changes; });
    env.doc->subscribePreview([&](DocStatePtr s, BuildResultPtr b) { previews.emplace_back(s, b); });

    const auto preview = addStep(2302, 'a', "grow", 3);
    env.doc->beginPreview(preview);
    REQUIRE(env.doc->previewState());
    CHECK_FALSE(env.doc->rebuildPending());
    REQUIRE(env.ui.runUntil([&] { return !previews.empty(); }));
    REQUIRE(env.doc->previewBuild());
    CHECK(previews.back().first == env.doc->previewState());
    CHECK(previews.back().second == env.doc->previewBuild());
    CHECK(ray::kernel::bounds(env.doc->previewBuild()->bodies.at(bodyId('a')).shape).max.x == 8.0);
    CHECK(env.doc->state() == state);
    CHECK(env.doc->build() == build);
    CHECK(changes == 0);

    // Committing the previewed command reuses the preview's work.
    const int built = BuildCounter::instance().count(featureId(2302));
    env.run(preview);
    CHECK_FALSE(env.doc->previewState());
    CHECK_FALSE(env.doc->previewBuild());
    REQUIRE(env.settle());
    CHECK(reportOf(*env.doc->build(), featureId(2302)).cacheHit);
    CHECK(BuildCounter::instance().count(featureId(2302)) == built);

    // A failing preview command has no build; cancelPreview ends a preview.
    env.doc->beginPreview(addStep(2302, 'a', "new", 1)); // id taken
    CHECK(env.doc->previewError());
    CHECK_FALSE(env.doc->previewBuild());
    env.doc->beginPreview(addStep(2303, 'a', "grow", 1));
    env.doc->cancelPreview();
    env.ui.runUntil([] { return false; }, std::chrono::milliseconds(200));
    CHECK_FALSE(env.doc->previewState());
}

TEST_CASE("listeners that throw are ignored and a document destroyed while rebuilding is safe",
          "[doc][rebuild][document]")
{
    auto env = std::make_unique<DocEnv>();
    env->doc->subscribe([](DocStatePtr, BuildResultPtr) { throw std::runtime_error("listener"); });
    env->run(addStep(2401, 'a', "new", 5));
    REQUIRE(env->settle());
    env->ui.runPending();
    CHECK(env->doc->build());

    env->run(addStep(2402, 'a', "grow", 1, R"(,"slowMs":20000)"));
    const auto start = std::chrono::steady_clock::now();
    env->doc.reset(); // cancels the rebuild; its continuation then finds no document
    env->ui.runUntil([] { return false; }, std::chrono::milliseconds(100));
    env.reset(); // the job system waits for the cancelled job
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
}
