// What the operation panel (task T-045) added to the CAM engine and the CAM state: the engine's notifications and the
// progress in its status, status on a worker that may outlive the engine, validate on a worker with the type's
// warnings, previews of snapshots that are not the document's (cached by input hash, never an operation's latest or
// running generation), and the setup's material and the operation's calculated-feeds record (stored, read back,
// checked by the commands, and in no input hash).
#include "engine_test_util.h"

#include "test_dispatcher.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <map>
#include <thread>

using Catch::Matchers::ContainsSubstring;
using namespace ray;
using namespace ray::cam;
using namespace ray::cam::test;

namespace {

// "warn.op": the test operation's toolpath, and a warning from check() (and in the toolpath) when its params say so.
class WarningOperation final : public IOperationType {
public:
    std::string_view typeName() const override { return "warn.op"; }
    std::string_view paramsSchemaJson() const override
    {
        return R"({"type":"object","properties":{"warn":{"type":"boolean","default":false}}})";
    }
    Status validate(const GenerateInput& in) const override { return inner_.validate(in); }
    Result<std::vector<std::string>> check(const GenerateInput& in) const override
    {
        if (in.op.paramsJson.find("\"warn\":true") != std::string::npos) {
            return std::vector<std::string>{"Mind the gap"};
        }
        return std::vector<std::string>{};
    }
    Result<Toolpath> generate(const GenerateInput& in, JobContext& ctx) const override
    {
        auto tp = inner_.generate(in, ctx);
        if (tp && in.op.paramsJson.find("\"warn\":true") != std::string::npos) {
            tp->warnings = {"Mind the gap"};
        }
        return tp;
    }

private:
    TestOperation inner_;
};

std::shared_ptr<const OperationRegistry> registry()
{
    auto r = std::make_shared<OperationRegistry>();
    r->add(std::make_unique<TestOperation>());
    r->add(std::make_unique<WarningOperation>());
    return r;
}

kernel::Shape box(double x, double y, double z)
{
    auto shape = kernel::makeBox(Vec3{0, 0, 0}, Vec3{x, y, z});
    REQUIRE(shape);
    return *shape;
}

std::string materialJson(std::uint64_t n)
{
    return R"({"schema":1,"id":")" + id('7', n) + R"(","name":"Alloy )" + std::to_string(n) +
           R"(","group":"Aluminium","hardnessHb":95,"specificCuttingForce":700,"density":2.7})";
}

// One part body (b1, a built box), a setup on it and operation c1 of the test type.
struct Env {
    ray::test::TestDispatcher ui;
    std::unique_ptr<IJobSystem> jobs = makeJobSystem(ui, 4);
    std::unique_ptr<ICamEngine> engine = makeCamEngine(*jobs, registry(), &doc::camSnapshot);
    doc::DocStatePtr state = std::make_shared<const doc::DocState>();
    doc::BuildResultPtr build;
    std::map<OperationId, int> notices;
    int listener = 0;

    Env()
    {
        REQUIRE(jobs);
        REQUIRE(engine);
        Gate::instance().reset();
        state = exec(state, command("body.set", R"({"id":")" + id('b', 1) + R"(","name":"Body","role":"part"})"));
        auto b = std::make_shared<doc::BuildResult>();
        const kernel::Shape shape = box(40, 20, 10);
        b->bodies[body('b', 1)] =
            doc::BuiltBody{shape, naming::nameImported(FeatureId{Uuid::fromString(id('f', 1))}, shape)};
        build = b;
        state = exec(state, command("cam.library.embed", library(toolJson(1), holderJson(1), machineJson(1))));
        state = exec(state, setupAdd(1, "\"" + id('b', 1) + "\""));
        state = exec(state, opAdd(1, 1, 1));
        listener = engine->subscribe([this](const OperationId& op) { ++notices[op]; });
        REQUIRE(listener != 0);
    }
    ~Env()
    {
        Gate::instance().open = true;
        engine.reset();
        jobs.reset();
    }

    template <class Done>
    bool until(Done done)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < deadline) {
            ui.runPending();
            if (done()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return done();
    }
    template <class T>
    Result<T> wait(JobHandle<T> job)
    {
        REQUIRE(job.valid());
        std::optional<Result<T>> out;
        job.then([&](Result<T> r) { out = std::move(r); });
        REQUIRE(until([&] { return out.has_value(); }));
        ui.runPending(); // the engine's own continuations
        return std::move(*out);
    }
    OpState statusOf(const doc::DocStatePtr& s, std::uint64_t op = 1) const
    {
        const auto all = engine->status(s, build);
        const auto it = all.find(opId(op));
        REQUIRE(it != all.end());
        return it->second;
    }
};

doc::Command opEdit(const std::string& members, std::uint64_t op = 1)
{
    return command("cam.operation.edit", R"({"id":")" + id('c', op) + "\"," + members + "}");
}

} // namespace

TEST_CASE("the engine notifies its listeners of a generation, with its progress in the status", "[cam][engine][t045]")
{
    Env e;
    Gate::instance().open = false;
    const doc::DocStatePtr blocking = exec(e.state, opEdit(R"("params":{"mode":"block"})"));
    auto job = e.engine->generate(blocking, e.build, opId(1));
    // Started: a notice arrives (posted, not inside generate()) and the status says Generating with a progress.
    CHECK(e.notices[opId(1)] == 0);
    REQUIRE(e.until([&] { return e.notices[opId(1)] >= 1 && Gate::instance().waiting.load() == 1; }));
    REQUIRE(e.until([&] { return e.statusOf(blocking).progress >= 0.0; }));
    const OpState generating = e.statusOf(blocking);
    CHECK(generating.status == OpStatus::Generating);
    CHECK(generating.progress >= 0.0);
    CHECK(generating.progress <= 1.0);
    // Ended: another notice, the status is up to date (and carries no progress).
    const int before = e.notices[opId(1)];
    Gate::instance().open = true;
    REQUIRE(e.wait(job));
    REQUIRE(e.until([&] { return e.notices[opId(1)] > before; }));
    CHECK(e.statusOf(blocking).status == OpStatus::UpToDate);
    CHECK(e.statusOf(blocking).progress == -1.0);

    // A generation found in the cache tells the listeners too when it becomes the latest toolpath.
    auto plain = e.wait(e.engine->generate(e.state, e.build, opId(1)));
    REQUIRE(plain);
    const int cachedBefore = e.notices[opId(1)];
    const auto again = e.wait(e.engine->generate(blocking, e.build, opId(1)));
    REQUIRE(again);
    REQUIRE(e.until([&] { return e.notices[opId(1)] > cachedBefore; }));

    // A failure and a cancel notify as well; unsubscribing stops the notices.
    const doc::DocStatePtr failing = exec(e.state, opEdit(R"("params":{"mode":"fail"})"));
    const int failBefore = e.notices[opId(1)];
    CHECK_FALSE(e.wait(e.engine->generate(failing, e.build, opId(1))));
    REQUIRE(e.until([&] { return e.notices[opId(1)] > failBefore; }));
    CHECK(e.statusOf(failing).status == OpStatus::Failed);
    e.engine->unsubscribe(e.listener);
    const int quiet = e.notices[opId(1)];
    const doc::DocStatePtr other = exec(e.state, opEdit(R"("heights":{"clearance":30})"));
    REQUIRE(e.wait(e.engine->generate(other, e.build, opId(1))));
    e.ui.runPending();
    CHECK(e.notices[opId(1)] == quiet);
    CHECK(e.engine->subscribe({}) == 0); // an empty listener is refused
}

TEST_CASE("status on a worker equals status() and outlives the engine", "[cam][engine][t045]")
{
    Env e;
    REQUIRE(e.wait(e.engine->generate(e.state, e.build, opId(1))));
    const doc::DocStatePtr stale = exec(e.state, opEdit(R"("heights":{"clearance":40})"));
    for (const auto& s : {e.state, stale}) {
        auto async = e.wait(e.engine->statusAsync(s, e.build));
        REQUIRE(async);
        const auto sync = e.engine->status(s, e.build);
        REQUIRE(async->size() == sync.size());
        for (const auto& [id, st] : sync) {
            CHECK(async->at(id).status == st.status);
            CHECK(async->at(id).message == st.message);
            CHECK(async->at(id).toolpath == st.toolpath);
        }
    }
    CHECK(e.wait(e.engine->statusAsync(e.state, e.build))->at(opId(1)).status == OpStatus::UpToDate);
    CHECK(e.wait(e.engine->statusAsync(stale, e.build))->at(opId(1)).status == OpStatus::Stale);

    // The job holds what it needs, not the engine: destroying the engine while it is queued is safe.
    Gate::instance().open = false;
    const doc::DocStatePtr blocking = exec(e.state, opEdit(R"("params":{"mode":"block"})"));
    std::vector<JobHandle<std::map<OperationId, OpState>>> queued;
    for (int i = 0; i < 16; ++i) {
        queued.push_back(e.engine->statusAsync(i % 2 == 0 ? e.state : blocking, e.build));
    }
    e.engine.reset();
    for (auto& job : queued) {
        auto r = job.wait();
        if (r) {
            CHECK(r->count(opId(1)) == 1);
        }
    }
}

TEST_CASE("validate runs the type's check on a worker: errors, warnings, blocked operations", "[cam][engine][t045]")
{
    Env e;
    // A valid operation that is not generated: Stale, no error, the type's warnings.
    const doc::DocStatePtr warned =
        exec(e.state, command("cam.operation.add", R"({"id":")" + id('c', 2) + R"(","setup":")" + id('a', 1) +
                                                       R"(","type":"warn.op","name":"Warned","tool":{"tool":")" +
                                                       id('d', 1) + R"("},"cutting":)" + kCutting + R"(,"heights":)" +
                                                       kHeights + R"(,"params":{"warn":true}})"));
    auto v = e.wait(e.engine->validate(warned, e.build, opId(2)));
    REQUIRE(v);
    CHECK(v->status == OpStatus::Stale);
    CHECK_FALSE(v->error.has_value());
    CHECK(v->warnings == std::vector<std::string>{"Mind the gap"});
    // validate changed nothing the engine holds.
    CHECK(e.statusOf(warned, 2).status == OpStatus::Stale);
    CHECK(e.statusOf(warned, 2).toolpath == nullptr);
    // Generated: UpToDate, and the warnings of the toolpath of these inputs.
    REQUIRE(e.wait(e.engine->generate(warned, e.build, opId(2))));
    v = e.wait(e.engine->validate(warned, e.build, opId(2)));
    REQUIRE(v);
    CHECK(v->status == OpStatus::UpToDate);
    CHECK(v->warnings == std::vector<std::string>{"Mind the gap"});

    // validate's error (the default check is validate with no warnings).
    const doc::DocStatePtr invalid = exec(e.state, opEdit(R"("params":{"mode":"invalid"})"));
    v = e.wait(e.engine->validate(invalid, e.build, opId(1)));
    REQUIRE(v);
    CHECK(v->status == OpStatus::Failed);
    REQUIRE(v->error.has_value());
    CHECK(v->error->message == "Test validation failed");
    const doc::DocStatePtr fine = e.state;
    v = e.wait(e.engine->validate(fine, e.build, opId(1)));
    REQUIRE(v);
    CHECK_FALSE(v->error.has_value());
    CHECK(v->warnings.empty());

    // Blocked before validation: suppressed, an unknown type, no build yet.
    const doc::DocStatePtr suppressed =
        exec(e.state, command("cam.operation.suppress", R"({"id":")" + id('c', 1) + R"(","suppressed":true})"));
    v = e.wait(e.engine->validate(suppressed, e.build, opId(1)));
    REQUIRE(v);
    CHECK(v->status == OpStatus::Suppressed);
    CHECK(v->error.has_value());
    const doc::DocStatePtr unknown = exec(e.state, opEdit(R"("type":"no.such.type")"));
    v = e.wait(e.engine->validate(unknown, e.build, opId(1)));
    REQUIRE(v);
    CHECK(v->status == OpStatus::Failed);
    REQUIRE(v->error.has_value());
    CHECK_THAT(v->error->message, ContainsSubstring("Unknown operation type"));
    v = e.wait(e.engine->validate(e.state, nullptr, opId(1)));
    REQUIRE(v);
    CHECK(v->status == OpStatus::Stale);
    CHECK_FALSE(v->error.has_value());
    // An operation the snapshot does not have.
    CHECK(e.wait(e.engine->validate(e.state, e.build, opId(9))).error().code == ErrorCode::NotFound);
}

TEST_CASE("a preview is generated on a worker, cached by input hash and never the operation's own",
          "[cam][engine][t045]")
{
    Env e;
    REQUIRE(e.wait(e.engine->generate(e.state, e.build, opId(1))));
    const auto shownBefore = e.statusOf(e.state).toolpath;
    REQUIRE(shownBefore != nullptr);

    // The panel's uncommitted values: a preview of them.
    const doc::DocStatePtr edited = exec(e.state, opEdit(R"("heights":{"clearance":33})"));
    const int noticesBefore = e.notices[opId(1)];
    auto preview = e.wait(e.engine->preview(edited, e.build, opId(1)));
    REQUIRE(preview);
    CHECK((*preview)->moves.front().z == toNm(33.0));
    REQUIRE(e.until([&] { return e.notices[opId(1)] > noticesBefore; })); // filed in the cache
    // The document's operation is untouched: still up to date with its own toolpath, which stays its latest.
    CHECK(e.statusOf(e.state).status == OpStatus::UpToDate);
    CHECK(e.statusOf(e.state).toolpath == shownBefore);
    // Committing the same values finds the preview's toolpath at once.
    CHECK(e.statusOf(edited).status == OpStatus::UpToDate);
    CHECK(e.statusOf(edited).toolpath == *preview);
    // A second preview of the same inputs is the cached toolpath.
    auto again = e.wait(e.engine->preview(edited, e.build, opId(1)));
    REQUIRE(again);
    CHECK(*again == *preview);

    // A running preview is not the operation's generation: the status does not say Generating; cancelling it ends it.
    Gate::instance().open = false;
    const doc::DocStatePtr blocking = exec(e.state, opEdit(R"("params":{"mode":"block"})"));
    auto running = e.engine->preview(blocking, e.build, opId(1));
    REQUIRE(e.until([] { return Gate::instance().waiting.load() == 1; }));
    CHECK(e.statusOf(blocking).status == OpStatus::Stale);
    CHECK(running.cancel());
    const auto cancelled = e.wait(running);
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == ErrorCode::Cancelled);
    Gate::instance().open = true;
    // A failing preview is not remembered as a failure of the operation.
    const doc::DocStatePtr failing = exec(e.state, opEdit(R"("params":{"mode":"fail"})"));
    CHECK_FALSE(e.wait(e.engine->preview(failing, e.build, opId(1))));
    CHECK(e.statusOf(failing).status == OpStatus::Stale);
    // A suppressed operation has no preview.
    const doc::DocStatePtr suppressed =
        exec(e.state, command("cam.operation.suppress", R"({"id":")" + id('c', 1) + R"(","suppressed":true})"));
    CHECK_FALSE(e.wait(e.engine->preview(suppressed, e.build, opId(1))));
}

TEST_CASE("the setup's material and the calculated-feeds record: stored, checked, in no input hash", "[cam][t045]")
{
    Env e;
    const auto hashOf = [&](const doc::DocStatePtr& s) {
        const auto snap = doc::camSnapshot(s, e.build);
        const auto resolver = naming::makeResolver(snap.bodies);
        auto h = operationInputHash(snap, *resolver, opId(1));
        REQUIRE(h);
        return *h;
    };
    const Hash256 plain = hashOf(e.state);

    // A material must be embedded (or come in the same step).
    const Error notEmbedded = applyFails(
        e.state, command("cam.setup.edit", R"({"id":")" + id('a', 1) + R"(","material":")" + id('7', 1) + R"("})"));
    CHECK_THAT(notEmbedded.message, ContainsSubstring("which is not embedded"));
    doc::DocStatePtr s =
        exec(e.state, command("cam.setup.edit", R"({"id":")" + id('a', 1) + R"(","material":")" + id('7', 1) +
                                                    R"(","library":{"materials":[)" + materialJson(1) + "]}}"));
    REQUIRE(s->cam->setups.front().material == MaterialId{Uuid::fromString(id('7', 1))});
    CHECK(hashOf(s) == plain);

    // The record of "Use calculated".
    const std::string record = R"({"tool":")" + id('d', 1) + R"(","material":")" + id('7', 1) + R"(","machine":")" +
                               id('9', 1) +
                               R"(","opClass":"drill","surfaceSpeed":null,"chipLoad":0.05,"rpm":1200,"feedCut":180,)"
                               R"("feedPlunge":90,"feedRamp":135})";
    s = exec(s, opEdit(R"("calculatedFeeds":)" + record));
    const cam::Operation& op = s->cam->operations.at(opId(1));
    REQUIRE(op.calculated.has_value());
    CHECK(op.calculated->tool == ToolId{Uuid::fromString(id('d', 1))});
    CHECK(op.calculated->opClass == "drill");
    CHECK_FALSE(op.calculated->surfaceSpeedMMin.has_value());
    CHECK(op.calculated->chipLoadMm == 0.05);
    CHECK(op.calculated->spindleRpm == 1200);
    CHECK(op.calculated->feedRamp == 135);
    CHECK(hashOf(s) == plain); // a record only: nothing goes stale
    const Error negative = applyFails(s, opEdit(R"("calculatedFeeds":{"tool":")" + id('d', 1) + R"(","rpm":-1})"));
    CHECK_THAT(negative.message, ContainsSubstring("calculatedFeeds.rpm"));

    // Stored and read back with the document; null forgets the record.
    const doc::StateObjects objects = doc::serializeState(*s);
    std::map<Hash256, std::string> byHash(objects.features.begin(), objects.features.end());
    auto back = doc::deserializeState(objects.manifest, [&](const Hash256& h) -> Result<std::string> {
        const auto it = byHash.find(h);
        if (it == byHash.end()) {
            return tl::unexpected(Error{ErrorCode::NotFound, "missing", {}});
        }
        return it->second;
    });
    REQUIRE(back);
    REQUIRE(back->cam);
    CHECK(back->cam->setups.front().material == s->cam->setups.front().material);
    REQUIRE(back->cam->operations.at(opId(1)).calculated.has_value());
    CHECK(back->cam->operations.at(opId(1)).calculated->feedCut == 180);
    CHECK(back->cam->operations.at(opId(1)).calculated->chipLoadMm == 0.05);
    CHECK(back->stateHash() == s->stateHash());
    const doc::DocStatePtr forgotten = exec(s, opEdit(R"("calculatedFeeds":null)"));
    CHECK_FALSE(forgotten->cam->operations.at(opId(1)).calculated.has_value());
    const doc::DocStatePtr noMaterial =
        exec(forgotten, command("cam.setup.edit", R"({"id":")" + id('a', 1) + R"(","material":null})"));
    CHECK(noMaterial->cam->setups.front().material == MaterialId{});
    // A document without either keeps its bytes: neither member is written when unset.
    const doc::StateObjects plainObjects = doc::serializeState(*e.state);
    bool mentions = false;
    for (const auto& [h, text] : plainObjects.features) {
        mentions = mentions || text.find("calculatedFeeds") != std::string::npos ||
                   text.find("\"material\"") != std::string::npos;
    }
    CHECK_FALSE(mentions);
}
