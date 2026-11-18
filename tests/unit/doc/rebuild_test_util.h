// Helpers shared by the rebuilder, import feature and document rebuild tests (task T-010).
//
// The test-only feature type "test.step" builds one box body and counts its builds per feature id:
//   props {"body": "<uuid>", "size": <mm>, "mode": "new" | "grow", "fail"?: true, "throw"?: true, "slowMs"?: n}
//   "new" makes a box of `size`; "grow" reads the body (shapeOf) and makes a box `size` longer in x than it was.
//   "fail" returns an error, "throw" throws, "slowMs" works that long first, checking cancellation every 2 ms.
#pragma once

#include "commands_test_util.h"
#include "json.h"
#include "test_dispatcher.h"

#include <ray/document.h>
#include <ray/kernel.h>
#include <ray/naming.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ray::doc::test {

class BuildCounter {
public:
    static BuildCounter& instance()
    {
        static BuildCounter counter;
        return counter;
    }
    void hit(const FeatureId& id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++counts_[id];
    }
    int count(const FeatureId& id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = counts_.find(id);
        return it == counts_.end() ? 0 : it->second;
    }

private:
    mutable std::mutex mutex_;
    std::map<FeatureId, int> counts_;
};

class StepFeature final : public IFeatureType {
public:
    std::string_view typeName() const override { return "test.step"; }
    std::string_view propsSchemaJson() const override { return R"({"type":"object"})"; }
    std::vector<naming::TopoRef> references(const Feature&) const override { return {}; }

    Result<FeatureOutput> rebuild(const Feature& feature, RebuildContext& ctx) const override
    {
        BuildCounter::instance().hit(feature.id);
        auto props = json::parse(feature.propsJson);
        if (!props || !props->isObject()) {
            return fail("bad props");
        }
        const json::Value* body = props->find("body");
        const json::Value* size = props->find("size");
        const json::Value* mode = props->find("mode");
        if (body == nullptr || size == nullptr || mode == nullptr) {
            return fail("missing props");
        }
        if (const json::Value* slow = props->find("slowMs")) {
            const auto until =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(slow->asNumber()));
            while (std::chrono::steady_clock::now() < until) {
                if (auto cancelled = ctx.job.cancel.check(); !cancelled) {
                    return tl::unexpected(cancelled.error());
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        if (props->find("throw") != nullptr) {
            throw std::runtime_error("test feature threw");
        }
        if (props->find("fail") != nullptr) {
            return fail("Test failure in " + feature.name);
        }
        const BodyId id{Uuid::fromString(body->asString())};
        double length = size->asNumber();
        if (mode->asString() == "grow") {
            auto before = ctx.shapeOf(id);
            if (!before) {
                return tl::unexpected(before.error());
            }
            (void)ctx.mapOf(id);
            length += kernel::bounds(*before).max.x;
        }
        auto shape = kernel::makeBox(Vec3{0, 0, 0}, Vec3{length, 10, 10});
        if (!shape) {
            return tl::unexpected(shape.error());
        }
        naming::ElementMap map = naming::nameImported(feature.id, *shape);
        FeatureOutput out;
        out.bodies.push_back(BodyUpdate{id, *shape, map, {}});
        return out;
    }

private:
    static tl::unexpected<Error> fail(std::string message)
    {
        return tl::unexpected(Error{ErrorCode::KernelFailure, std::move(message), {}});
    }
};

inline std::shared_ptr<const FeatureRegistry> testRegistry()
{
    auto registry = std::make_shared<FeatureRegistry>();
    REQUIRE(registerBuiltinFeatureTypes(*registry));
    registry->add(std::make_unique<StepFeature>());
    return registry;
}

inline std::string stepProps(char body, const std::string& mode, double size, const std::string& extra = {})
{
    return R"({"body":")" + uuid(body, 1) + R"(","mode":")" + mode + R"(","size":)" + std::to_string(size) + extra +
           "}";
}

inline Feature stepFeature(std::uint64_t n, char body, const std::string& mode, double size,
                           const std::string& extra = {})
{
    Feature f;
    f.id = FeatureId{Uuid::fromString(uuid('f', n))};
    f.type = "test.step";
    f.name = "Step " + std::to_string(n);
    f.propsJson = canonicalJson(stepProps(body, mode, size, extra));
    return f;
}

inline FeatureId featureId(std::uint64_t n)
{
    return FeatureId{Uuid::fromString(uuid('f', n))};
}

inline BodyId bodyId(char body)
{
    return BodyId{Uuid::fromString(uuid(body, 1))};
}

inline Command addStep(std::uint64_t n, char body, const std::string& mode, double size, const std::string& extra = {})
{
    return cmd("feature.add",
               R"({"id":")" + uuid('f', n) + R"(","type":"test.step","name":"Step )" + std::to_string(n) +
                   R"(","props":)" + stepProps(body, mode, size, extra) + "}",
               "Add Step " + std::to_string(n));
}

inline std::string dataFile(const char* name)
{
    return std::string(RAY_DOC_KERNEL_DATA_DIR) + "/" + name;
}

// A job system, an object pool, the test registry and a rebuilder over them.
struct RebuildEnv {
    explicit RebuildEnv(int workers = 4, std::shared_ptr<ObjectPool> objects = makeObjectPool())
        : jobs(makeJobSystem(ui, workers)), pool(std::move(objects)), types(testRegistry()),
          rebuilder(makeRebuilder(*jobs, types, pool))
    {
        REQUIRE(jobs);
        REQUIRE(pool);
        REQUIRE(rebuilder);
    }

    BuildResultPtr rebuild(const DocStatePtr& state)
    {
        JobContext ctx;
        auto result = rebuilder->rebuild(state, ctx);
        if (!result) {
            FAIL("rebuild failed: " << result.error().message);
        }
        REQUIRE(*result);
        return *result;
    }

    ray::test::TestDispatcher ui;
    std::unique_ptr<IJobSystem> jobs;
    std::shared_ptr<ObjectPool> pool;
    std::shared_ptr<const FeatureRegistry> types;
    std::shared_ptr<IRebuilder> rebuilder;
};

// A document on that environment.
struct DocEnv : RebuildEnv {
    explicit DocEnv(int workers = 4, DocStatePtr initial = {}, std::shared_ptr<ObjectPool> objects = makeObjectPool())
        : RebuildEnv(workers, std::move(objects)), doc(makeDocument(ui, *jobs, rebuilder, {}, std::move(initial)))
    {
        REQUIRE(doc);
    }
    ~DocEnv() { doc.reset(); }

    // Runs the dispatcher until no rebuild is pending.
    bool settle(std::chrono::milliseconds timeout = std::chrono::milliseconds(60000))
    {
        return ui.runUntil([&] { return !doc->rebuildPending(); }, timeout);
    }

    DocStatePtr run(const Command& command)
    {
        auto result = doc->execute(command);
        if (!result) {
            FAIL_CHECK(command.name << " failed: " << result.error().message);
            return nullptr;
        }
        return *result;
    }

    // prepareImport, objects into the pool, then execute the command.
    DocStatePtr import(const std::string& path, std::uint64_t n)
    {
        ImportRequest request;
        request.feature = featureId(n);
        request.path = path;
        JobContext ctx;
        auto prepared = prepareImport(request, ctx);
        if (!prepared) {
            FAIL("prepareImport failed: " << prepared.error().message);
        }
        for (const auto& [hash, bytes] : prepared->objects) {
            auto put = pool->put(bytes);
            REQUIRE(put);
            REQUIRE(*put == hash);
        }
        return run(prepared->command);
    }

    std::unique_ptr<Document> doc;
};

inline const FeatureReport& reportOf(const BuildResult& build, const FeatureId& id)
{
    for (const FeatureReport& r : build.reports) {
        if (r.id == id) {
            return r;
        }
    }
    FAIL_CHECK("no report for feature " << id.value.toString());
    static const FeatureReport missing{FeatureId{}, FeatureStatus::NotBuilt, "no report", 0.0, false};
    return missing;
}

// Everything about a build that must not depend on the worker count: statuses and every body's geometry and names.
inline std::string fingerprint(const DocState& state, const BuildResult& build)
{
    Hasher h;
    h.str(state.stateHash().hex());
    for (const FeatureReport& r : build.reports) {
        h.str(r.id.value.toString());
        h.i64(static_cast<std::int64_t>(r.status));
        h.str(r.message);
    }
    for (const auto& [id, body] : build.bodies) {
        h.str(id.value.toString());
        h.str(body.shape.contentHash().hex());
        h.bytes(body.map.serialize());
    }
    return h.finish().hex();
}

// A folder of its own under the system temp folder, removed afterwards.
struct TempDir {
    TempDir()
    {
        const auto base = std::filesystem::temp_directory_path();
        for (int i = 0; i < 1000; ++i) {
            path = base / ("ray-doc-test-" + Uuid::generate().toString());
            std::error_code ec;
            if (std::filesystem::create_directory(path, ec)) {
                return;
            }
        }
        FAIL("cannot create a temporary folder");
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string file(const char* name) const // UTF-8, as the kernel and the import props take paths
    {
        const std::u8string text = (path / name).u8string();
        return std::string(text.begin(), text.end());
    }
    std::filesystem::path path;
};

} // namespace ray::doc::test
