// In-process stock through the CAM engine (task T-075), with a stand-in simulator (CamEngineOptions::simulateSetup)
// that counts its runs and can be held: setup 2 starts from what setup 1 leaves, flipped (its Z face is the part's
// bottom); the carried stock is made once and kept, whatever regenerates; exactly the edits that can change it make
// setup 2 stale; jobs that need it at once wait for one simulation; a cancelled one is made again by the next job; a
// failure of setup 1 names the operation; one worker gets through; ICamEngine::carriedStock hands out the same stock.
#include "engine_test_util.h"
#include "test_dispatcher.h"

#include <ray/cam.h>
#include <ray/document.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

using Catch::Matchers::ContainsSubstring;
using namespace ray;
using namespace ray::cam;
using namespace ray::cam::test;

namespace {

// ---------- An operation type that records the geometry it was given ----------
struct Seen {
    Box3 stockBounds;
    std::optional<std::vector<std::pair<double, std::vector<kernel::ProfileLoop>>>> slices;
};
struct Probe {
    std::mutex mutex;
    std::map<OperationId, Seen> seen;
    static Probe& instance()
    {
        static Probe p;
        return p;
    }
};

class ProbeOperation final : public IOperationType {
public:
    std::string_view typeName() const override { return "test.probe"; }
    std::string_view paramsSchemaJson() const override
    {
        return R"({"type":"object","properties":{"mode":{"enum":["plain"],"default":"plain"}}})";
    }
    Status validate(const GenerateInput&) const override { return {}; }
    Result<Toolpath> generate(const GenerateInput& in, JobContext&) const override
    {
        {
            Probe& p = Probe::instance();
            std::lock_guard<std::mutex> lock(p.mutex);
            p.seen[in.op.id] = Seen{in.geom.stockBounds, in.geom.stockSlices};
        }
        // A path down to the stock's bottom, so the toolpath depends on the stock it was given.
        Toolpath tp{};
        Move a{};
        a.kind = MoveKind::Rapid;
        a.role = MoveRole::Approach;
        a.z = toNm(in.geom.stockBounds.max.z + 5);
        Move b{};
        b.kind = MoveKind::Linear;
        b.role = MoveRole::Cut;
        b.feedMicronPerMin = 1000;
        b.x = toNm(in.geom.stockBounds.max.x);
        b.z = toNm(in.geom.stockBounds.min.z);
        tp.moves = {a, b};
        return tp;
    }
};

// ---------- The stand-in simulator: setup 1's stock with its top `faced` mm lower ----------
struct FakeSimulator {
    std::atomic<int> calls{0};
    std::atomic<int> inside{0};
    std::atomic<bool> open{true};
    std::mutex mutex;
    std::vector<OperationId> lastOps;
    double faced = 2.0;

    Result<kernel::TriSoup> run(const SimulateSetupRequest& r, JobContext& ctx)
    {
        ++calls;
        ++inside;
        {
            std::lock_guard<std::mutex> lock(mutex);
            lastOps.clear();
            for (const auto& t : r.toolpaths) {
                lastOps.push_back(t->operation);
            }
        }
        while (!open.load()) {
            if (auto st = ctx.cancel.check(); !st) {
                --inside;
                return tl::unexpected(st.error());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        --inside;
        CHECK(r.cellMm == kCarriedStockCellMm);
        const Box3& b = r.resolved.stockBounds;
        kernel::TriSoup s;
        const Vec3 lo = b.min, hi{b.max.x, b.max.y, b.max.z - faced};
        for (int k = 0; k < 8; ++k) {
            s.positions.push_back((k & 1) ? hi.x : lo.x);
            s.positions.push_back((k & 2) ? hi.y : lo.y);
            s.positions.push_back((k & 4) ? hi.z : lo.z);
        }
        s.indices = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4,
                     2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
        return s;
    }
};

kernel::Shape box(double x, double y, double z)
{
    auto shape = kernel::makeBox(Vec3{0, 0, 0}, Vec3{x, y, z});
    REQUIRE(shape);
    return *shape;
}

doc::BuiltBody built(std::uint64_t feature, const kernel::Shape& shape)
{
    return doc::BuiltBody{shape, naming::nameImported(FeatureId{Uuid::fromString(id('f', feature))}, shape)};
}

doc::Command probeAdd(std::uint64_t n, std::uint64_t setup)
{
    return command("cam.operation.add", R"({"id":")" + id('c', n) + R"(","setup":")" + id('a', setup) +
                                            R"(","type":"test.probe","name":"Probe )" + std::to_string(n) +
                                            R"(","tool":{"tool":")" + id('d', 1) + R"("},"cutting":{"rpm":)" +
                                            std::to_string(8000 + n) +
                                            R"(,"feedCut":1200,"feedPlunge":300},"heights":)" + kHeights +
                                            R"(,"params":{"mode":"plain"}})"); // each its own input hash
}

// The 40 x 20 x 10 block b1 (and b2, a vise stand-in); setup 1 on b1 (box stock 2 above, 1 below) with operations
// c1, c2; setup 2 on b1 from setup 1's stock, its Z face b1's bottom (turned over about X), probes c3, c4, c5.
struct Job {
    doc::DocStatePtr state = std::make_shared<const doc::DocState>();
    doc::BuildResultPtr build;

    Job()
    {
        for (int n = 1; n <= 2; ++n) {
            state = exec(state, command("body.set", R"({"id":")" + id('b', n) + R"(","name":"Body )" +
                                                        std::to_string(n) + R"(","role":"part"})"));
        }
        auto b = std::make_shared<doc::BuildResult>();
        b->bodies[body('b', 1)] = built(1, box(40, 20, 10));
        b->bodies[body('b', 2)] = built(2, box(5, 5, 5));
        build = b;
        const auto snap = doc::camSnapshot(state, build);
        const auto resolver = naming::makeResolver(snap.bodies);
        const kernel::Shape& a = build->bodies.at(body('b', 1)).shape;
        const auto bottom = findFace(a, [](const kernel::SurfaceInfo& s) { return s.centroid.z < 0.01; });
        const std::string bottomRef = naming::toJson(resolver->makeRef(body('b', 1), bottom));
        state = exec(state, command("cam.library.embed", library(toolJson(1), holderJson(1), machineJson(1))));
        state = exec(state, setupAdd(1, "\"" + id('b', 1) + "\"", R"(,"stock":{"type":"box","zPos":2,"zNeg":1})"));
        state = exec(state, opAdd(1, 1, 1));
        state = exec(state, opAdd(2, 1, 1, "[]", R"({"mode":"plain","depth":1})"));
        state =
            exec(state, setupAdd(2, "\"" + id('b', 1) + "\"",
                                 R"(,"stock":{"type":"fromSetup","setup":")" + id('a', 1) + R"("},"wcs":{"zFace":)" +
                                     bottomRef + R"(,"origin":{"mode":"stockCorner","corner":0},"workOffset":2})"));
        for (std::uint64_t n = 3; n <= 5; ++n) {
            state = exec(state, probeAdd(n, 2));
        }
    }
    void run(const doc::Command& c) { state = exec(state, c); }
};

std::shared_ptr<const OperationRegistry> registry()
{
    auto r = std::make_shared<OperationRegistry>();
    r->add(std::make_unique<TestOperation>());
    r->add(std::make_unique<ProbeOperation>());
    return r;
}

struct Env {
    ray::test::TestDispatcher ui;
    std::unique_ptr<IJobSystem> jobs;
    FakeSimulator fake;
    std::unique_ptr<ICamEngine> engine;

    explicit Env(int workers = 4, bool simulate = true) : jobs(makeJobSystem(ui, workers))
    {
        REQUIRE(jobs);
        CamEngineOptions options;
        if (simulate) {
            options.simulateSetup = [this](const SimulateSetupRequest& r, JobContext& ctx) {
                return fake.run(r, ctx);
            };
        }
        engine = makeCamEngine(*jobs, registry(), &doc::camSnapshot, std::move(options));
        REQUIRE(engine);
        Gate::instance().reset();
    }
    ~Env()
    {
        fake.open = true;
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

    OpStatus statusOf(const Job& j, std::uint64_t op)
    {
        const auto all = engine->status(j.state, j.build);
        const auto it = all.find(opId(op));
        REQUIRE(it != all.end());
        INFO(it->second.message);
        return it->second.status;
    }

    struct Pending {
        JobHandle<std::shared_ptr<const Toolpath>> job;
        std::shared_ptr<std::optional<Result<std::shared_ptr<const Toolpath>>>> result;
    };
    Pending start(const Job& j, std::uint64_t op)
    {
        Pending p{engine->generate(j.state, j.build, opId(op)),
                  std::make_shared<std::optional<Result<std::shared_ptr<const Toolpath>>>>()};
        auto slot = p.result;
        p.job.then([slot](Result<std::shared_ptr<const Toolpath>> r) { *slot = std::move(r); });
        return p;
    }
    Result<std::shared_ptr<const Toolpath>> finish(Pending& p)
    {
        REQUIRE(until([&] { return p.result->has_value(); }));
        return **p.result;
    }
    Result<std::shared_ptr<const Toolpath>> generate(const Job& j, std::uint64_t op)
    {
        Pending p = start(j, op);
        return finish(p);
    }
    std::shared_ptr<const Toolpath> generated(const Job& j, std::uint64_t op)
    {
        auto r = generate(j, op);
        if (!r) {
            FAIL("generation of op " << op << " failed: " << r.error().message);
        }
        return *r;
    }
};

Seen seenBy(std::uint64_t op)
{
    Probe& p = Probe::instance();
    std::lock_guard<std::mutex> lock(p.mutex);
    const auto it = p.seen.find(opId(op));
    REQUIRE(it != p.seen.end());
    return it->second;
}

doc::Command opEdit(std::uint64_t op, const std::string& members)
{
    return command("cam.operation.edit", R"({"id":")" + id('c', op) + "\"," + members + "}");
}

doc::Command setupEdit(std::uint64_t setup, const std::string& members)
{
    return command("cam.setup.edit", R"({"id":")" + id('a', setup) + "\"," + members + "}");
}

} // namespace

TEST_CASE("setup 2 starts from the stock setup 1 leaves, turned over, its slices filled", "[cam][carried][engine]")
{
    Job j;
    Env e;
    CHECK(e.statusOf(j, 3) == OpStatus::Stale);
    e.generated(j, 3);
    CHECK(e.fake.calls == 1);
    {
        std::lock_guard<std::mutex> lock(e.fake.mutex);
        CHECK(e.fake.lastOps == std::vector<OperationId>{opId(1), opId(2)}); // setup 1's operations, in order
    }
    // Setup 1's stock: 40 x 20 x 13 (2 above the block, 1 below); its end 2 lower on top. Turned over, the stock is
    // 11 high, its top the old bottom (uncut, exact), its bottom the faced top; corner 0 is the origin.
    const Seen s = seenBy(3);
    CHECK(s.stockBounds.min.x == 0.0);
    CHECK(s.stockBounds.min.y == 0.0);
    CHECK(s.stockBounds.min.z == -11.0);
    CHECK(s.stockBounds.max.x == 40.0);
    CHECK(s.stockBounds.max.y == 20.0);
    CHECK(s.stockBounds.max.z == 0.0);
    REQUIRE(s.slices.has_value());
    REQUIRE(s.slices->size() == 2);
    CHECK((*s.slices)[0].first == -11.0);
    CHECK((*s.slices)[0].second.size() == 1); // the whole 40 x 20
    CHECK((*s.slices)[1].first == 0.0);
    CHECK((*s.slices)[1].second.empty()); // nothing above the top
    // Setup 1's toolpaths were made on the way, and filed: those operations are up to date.
    CHECK(e.statusOf(j, 1) == OpStatus::UpToDate);
    CHECK(e.statusOf(j, 2) == OpStatus::UpToDate);
    CHECK(e.statusOf(j, 3) == OpStatus::UpToDate);
    CHECK(e.statusOf(j, 4) == OpStatus::Stale);

    // The other operations of setup 2, and a regeneration of an edited one, do not simulate again.
    e.generated(j, 4);
    j.run(opEdit(3, R"("cutting":{"rpm":7000})"));
    CHECK(e.statusOf(j, 3) == OpStatus::Stale);
    e.generated(j, 3);
    CHECK(e.fake.calls == 1);
    // A deeper cut in setup 1 (the stand-in faces 3): only after setup 1 changes does setup 2 see it.
    e.fake.faced = 3.0;
    e.generated(j, 5);
    CHECK(seenBy(5).stockBounds.min.z == -11.0); // the kept stock
    CHECK(e.fake.calls == 1);
}

TEST_CASE("a chain: setup 3 starts from what setup 2 leaves of setup 1's stock", "[cam][carried][engine]")
{
    Job j;
    // Setup 3 turned back over (the model's axes, corner 0 on top), from setup 2.
    j.run(setupAdd(3, "\"" + id('b', 1) + "\"", R"(,"stock":{"type":"fromSetup","setup":")" + id('a', 2) + R"("})"));
    j.run(probeAdd(6, 3));
    Env e;
    e.generated(j, 6);
    // Setup 1's stock (model Z -1..12) less 2 on top (setup 1), then 2 on its underside (setup 2): model Z 1..10.
    const Seen s = seenBy(6);
    CHECK(s.stockBounds.min.z == -9.0);
    CHECK(s.stockBounds.max.z == 0.0);
    CHECK(s.stockBounds.max.x == 40.0);
    CHECK(s.stockBounds.max.y == 20.0);
    CHECK(e.fake.calls == 2); // setup 1 once, setup 2 once
    for (const std::uint64_t op : {1, 2, 3, 4, 5}) {
        CHECK(e.statusOf(j, op) == OpStatus::UpToDate); // setups 1 and 2, made on the way
    }
    // An edit of setup 1 reaches setup 3 through setup 2.
    j.run(opEdit(1, R"("cutting":{"feedCut":900})"));
    CHECK(e.statusOf(j, 6) == OpStatus::Stale);
}

TEST_CASE("exactly the edits that can change setup 1's stock make setup 2 stale", "[cam][carried][engine]")
{
    Job j;
    Env e;
    e.generated(j, 3);
    REQUIRE(e.statusOf(j, 3) == OpStatus::UpToDate);
    const doc::DocStatePtr start = j.state;
    int calls = 1;
    // Setup 2's own frame: its operations go stale, but setup 1's stock is the same, so nothing is simulated.
    j.run(setupEdit(2, R"("wcs":{"flipX":true,"origin":{"mode":"stockCorner","corner":0},"workOffset":2})"));
    CHECK(e.statusOf(j, 3) == OpStatus::Stale);
    e.generated(j, 3);
    CHECK(e.fake.calls == calls);

    const auto stale = [&](const doc::Command& c, const char* what) {
        INFO(what);
        j.state = start;
        j.run(c);
        CHECK(e.statusOf(j, 3) == OpStatus::Stale);
        CHECK(e.statusOf(j, 4) == OpStatus::Stale);
        e.generated(j, 3); // simulated again: the carried stock's key changed
        CHECK(e.fake.calls == ++calls);
    };
    const auto kept = [&](const doc::Command& c, const char* what) {
        INFO(what);
        j.state = start;
        j.run(c);
        CHECK(e.statusOf(j, 3) == OpStatus::UpToDate);
        CHECK(e.fake.calls == calls);
    };
    stale(opEdit(1, R"("params":{"mode":"plain","depth":2})"), "a setup 1 operation's parameters");
    stale(opEdit(1, R"("tool":{"stickOut":30})"), "its tool");
    stale(opEdit(1, R"("cutting":{"feedCut":900})"), "its cutting data");
    stale(command("cam.operation.suppress", R"({"id":")" + id('c', 2) + R"(","suppressed":true})"),
          "a suppression in setup 1");
    stale(command("cam.operation.reorder", R"({"id":")" + id('c', 2) + R"(","index":0})"), "setup 1's order");
    stale(setupEdit(1, R"("stock":{"type":"box","zPos":3,"zNeg":1})"), "setup 1's stock");
    stale(setupEdit(1, R"("fixtureBodies":[")" + id('b', 2) + R"("])"),
          "a fixture of setup 1 (the generators keep clear of fixtures, so its toolpaths may change)");
    stale(command("cam.operation.add", R"({"id":")" + id('c', 9) + R"(","setup":")" + id('a', 1) +
                                           R"(","type":"test.op","name":"Op 9","tool":{"tool":")" + id('d', 1) +
                                           R"("},"cutting":)" + kCutting + R"(,"heights":)" + kHeights + "}"),
          "an operation added to setup 1");

    kept(opEdit(1, R"("name":"Renamed")"), "a setup 1 operation's name");
    kept(setupEdit(1, R"("programNumber":2002,"name":"Op 1 - top")"), "setup 1's program number and name");
    kept(opEdit(4, R"("cutting":{"rpm":7000})"), "another operation of setup 2");

    // Back to the start (an undo): up to date at once, from the cache.
    j.state = start;
    CHECK(e.statusOf(j, 3) == OpStatus::UpToDate);
    CHECK(e.fake.calls == calls);
}

TEST_CASE("operations that need the same stock at once wait for one simulation", "[cam][carried][engine]")
{
    Job j;
    Env e;
    e.fake.open = false;
    std::vector<Env::Pending> pending;
    for (std::uint64_t op = 3; op <= 5; ++op) {
        pending.push_back(e.start(j, op));
    }
    REQUIRE(e.until([&] { return e.fake.inside.load() == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    e.ui.runPending();
    CHECK(e.fake.calls == 1);
    e.fake.open = true;
    for (Env::Pending& p : pending) {
        auto r = e.finish(p);
        CHECK(r.has_value());
    }
    CHECK(e.fake.calls == 1);
    CHECK(seenBy(3).stockBounds.min.z == seenBy(5).stockBounds.min.z);
}

TEST_CASE("a cancelled simulation is made again by the next job that needs it", "[cam][carried][engine]")
{
    Job j;
    Env e;
    e.fake.open = false;
    Env::Pending first = e.start(j, 3);
    REQUIRE(e.until([&] { return e.fake.inside.load() == 1; }));
    Env::Pending second = e.start(j, 4); // waits for the first
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(e.fake.calls == 1);
    CHECK(first.job.cancel());
    auto r1 = e.finish(first);
    REQUIRE_FALSE(r1.has_value());
    CHECK(r1.error().code == ErrorCode::Cancelled);
    CHECK(e.statusOf(j, 3) == OpStatus::Stale);
    // The waiting job makes it now.
    REQUIRE(e.until([&] { return e.fake.calls.load() == 2 && e.fake.inside.load() == 1; }));
    e.fake.open = true;
    auto r2 = e.finish(second);
    CHECK(r2.has_value());
    e.generated(j, 3);
    CHECK(e.fake.calls == 2);
}

TEST_CASE("a failure in setup 1 fails setup 2 naming it; without a simulator setup 2 does not resolve",
          "[cam][carried][engine]")
{
    SECTION("a failing operation in setup 1")
    {
        Job j;
        Env e;
        j.run(opEdit(2, R"("params":{"mode":"fail"})"));
        auto r = e.generate(j, 3);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ErrorCode::ToolpathFailed);
        CHECK_THAT(r.error().message, ContainsSubstring("'Op 2', which failed"));
        CHECK_THAT(r.error().message, ContainsSubstring("Test generator failed"));
        CHECK(e.statusOf(j, 2) == OpStatus::Failed);
        CHECK(e.statusOf(j, 3) == OpStatus::Failed);
        CHECK(e.fake.calls == 0);
    }
    SECTION("an engine without simulateSetup")
    {
        Job j;
        Env e(4, false);
        auto r = e.generate(j, 3);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ErrorCode::InvalidArgument);
        CHECK_THAT(r.error().message, ContainsSubstring("must be simulated first"));
        e.generated(j, 1); // setup 1 is not affected
    }
}

TEST_CASE("one worker generates both setups at once; carriedStock hands out the kept stock", "[cam][carried][engine]")
{
    Job j;
    Env e(1);
    e.engine->generateAllStale(j.state, j.build);
    REQUIRE(e.until([&] {
        for (std::uint64_t op = 1; op <= 5; ++op) {
            if (e.statusOf(j, op) != OpStatus::UpToDate) {
                return false;
            }
        }
        return true;
    }));
    CHECK(e.fake.calls == 1);

    auto job = e.engine->carriedStock(j.state, j.build, SetupId{Uuid::fromString(id('a', 2))});
    std::optional<Result<std::shared_ptr<const CarriedStock>>> got;
    job.then([&](Result<std::shared_ptr<const CarriedStock>> r) { got = std::move(r); });
    REQUIRE(e.until([&] { return got.has_value(); }));
    REQUIRE(got->has_value());
    const CarriedStock& c = ***got;
    CHECK(c.setup == SetupId{Uuid::fromString(id('a', 1))});
    CHECK(c.cellMm == kCarriedStockCellMm);
    CHECK(c.bounds.min.z == -13.0);
    REQUIRE(c.solid);
    CHECK(c.solid->indices.size() == 36);
    CHECK(e.fake.calls == 1);
    // Setup 1 has no carried stock.
    auto none = e.engine->carriedStock(j.state, j.build, SetupId{Uuid::fromString(id('a', 1))});
    std::optional<Result<std::shared_ptr<const CarriedStock>>> refused;
    none.then([&](Result<std::shared_ptr<const CarriedStock>> r) { refused = std::move(r); });
    REQUIRE(e.until([&] { return refused.has_value(); }));
    REQUIRE_FALSE(refused->has_value());
    CHECK(refused->error().code == ErrorCode::InvalidArgument);
}
