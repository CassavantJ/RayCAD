// LibraryStack::refreshInBackground and sharedLibrary (added for the tool library UI, T-046): a rescan whose folder
// is slow does not hold up the thread that asked for it, its result is merged on the UI thread, a root that comes
// back is opened, new roots drop a running rescan, and a shared library outlives the roots it came from.
#include "lib_test_util.h"
#include "library_internal.h"
#include "test_dispatcher.h"

#include <ray/libraries.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

using namespace libtest;
using ray::lib::LibraryStack;

namespace {

void saveAll(const std::string& root, const std::vector<ray::lib::Tool>& tools)
{
    auto library = ray::lib::ILibrary::open(root);
    REQUIRE(library);
    for (const auto& t : tools) {
        REQUIRE((*library)->save(t));
    }
}

} // namespace

TEST_CASE("refreshInBackground rescans on a worker and merges on the UI thread", "[lib][stack][background]")
{
    TempDir builtin;
    TempDir share;
    TempDir user;
    saveAll(builtin.dir(), {sampleTool(1)});
    saveAll(share.dir(), {sampleTool(2)});
    ray::test::TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 4);
    REQUIRE(jobs);
    // The share's rescan blocks until released (or cancelled), as a slow network folder would.
    std::atomic<bool> release{false};
    std::atomic<int> blocked{0};
    std::atomic<int> cancelled{0};
    ray::lib::detail::setBackgroundReadHook([&](const std::string& dir, const ray::CancelToken& cancel) {
        if (dir != share.dir()) {
            return;
        }
        ++blocked;
        while (!release.load()) {
            if (cancel.cancelled()) {
                ++cancelled;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    {
        LibraryStack stack;
        stack.setRoots({builtin.dir(), share.dir(), user.dir()}, 1);
        int calls = 0;
        stack.subscribe([&calls] { ++calls; });
        // Another writer adds a tool to the share.
        saveAll(share.dir(), {sampleTool(3)});

        std::optional<bool> done;
        const auto start = std::chrono::steady_clock::now();
        stack.refreshInBackground(*jobs, [&done](bool changed) { done = changed; });
        const auto took = std::chrono::steady_clock::now() - start;
        CHECK(took < std::chrono::milliseconds(500)); // returns at once although the share's rescan is blocked
        REQUIRE(dispatcher.runUntil([&] { return blocked.load() == 1; }, std::chrono::milliseconds(10000)));
        // Nothing merged yet: the UI thread is free and the stack unchanged.
        dispatcher.runUntil([] { return false; }, std::chrono::milliseconds(50));
        CHECK_FALSE(done.has_value());
        CHECK(stack.tool(ray::ToolId{fixedId(3)}) == nullptr);
        CHECK(calls == 0);

        release = true;
        CHECK(dispatcher.runUntil([&] { return done.has_value(); }, std::chrono::milliseconds(10000)));
        CHECK(done == std::optional<bool>(true));
        CHECK(calls == 1);
        CHECK(stack.tool(ray::ToolId{fixedId(3)}) != nullptr);
        CHECK(stack.rootOf(ray::lib::ItemKind::Tool, fixedId(3)) == std::optional<std::size_t>(1));

        // Nothing new: done(false), no listener call.
        done.reset();
        stack.refreshInBackground(*jobs, [&done](bool changed) { done = changed; });
        CHECK(dispatcher.runUntil([&] { return done.has_value(); }, std::chrono::milliseconds(10000)));
        CHECK(done == std::optional<bool>(false));
        CHECK(calls == 1);

        // New roots while a rescan is blocked: it is cancelled and its result dropped (done is not called).
        release = false;
        blocked = 0;
        done.reset();
        stack.refreshInBackground(*jobs, [&done](bool changed) { done = changed; });
        REQUIRE(dispatcher.runUntil([&] { return blocked.load() == 1; }, std::chrono::milliseconds(10000)));
        stack.setRoots({builtin.dir()}, 1);
        CHECK(dispatcher.runUntil([&] { return cancelled.load() == 1; }, std::chrono::milliseconds(10000)));
        dispatcher.runUntil([] { return false; }, std::chrono::milliseconds(100));
        CHECK_FALSE(done.has_value());
        CHECK(stack.roots().size() == 1);
    }
    dispatcher.runUntil([] { return false; }, std::chrono::milliseconds(100));
    jobs.reset();
    ray::lib::detail::setBackgroundReadHook({});
}

TEST_CASE("refreshInBackground opens a root that was missing and is there now", "[lib][stack][background]")
{
    TempDir builtin;
    TempDir parent;
    saveAll(builtin.dir(), {sampleTool(1)});
    const std::string later = parent.path("comes later");
    ray::test::TestDispatcher dispatcher;
    auto jobs = ray::makeJobSystem(dispatcher, 2);
    REQUIRE(jobs);
    {
        LibraryStack stack;
        stack.setRoots({builtin.dir(), later}, 1);
        REQUIRE(stack.issues().size() == 1);
        CHECK_FALSE(stack.roots()[1].open);
        std::filesystem::create_directories(std::filesystem::path(std::u8string(later.begin(), later.end())));
        saveAll(later, {sampleTool(4)});
        std::optional<bool> done;
        stack.refreshInBackground(*jobs, [&done](bool changed) { done = changed; });
        CHECK(dispatcher.runUntil([&] { return done.has_value(); }, std::chrono::milliseconds(10000)));
        CHECK(done == std::optional<bool>(true));
        CHECK(stack.roots()[1].open);
        CHECK(stack.roots()[1].writable);
        CHECK(stack.issues().empty());
        CHECK(stack.tool(ray::ToolId{fixedId(4)}) != nullptr);
    }
    dispatcher.runUntil([] { return false; }, std::chrono::milliseconds(100));
    jobs.reset();
}

TEST_CASE("sharedLibrary is null where library() is and outlives the roots it came from", "[lib][stack]")
{
    TempDir builtin;
    TempDir user;
    saveAll(builtin.dir(), {sampleTool(1)});
    LibraryStack stack;
    stack.setRoots({builtin.dir(), user.dir()}, 1);
    CHECK(stack.sharedLibrary(0) == nullptr); // read-only
    CHECK(stack.sharedLibrary(2) == nullptr); // past the end
    std::shared_ptr<ray::lib::ILibrary> kept = stack.sharedLibrary(1);
    REQUIRE(kept != nullptr);
    CHECK(kept.get() == stack.library(1));
    // New roots replace the libraries; the shared one still saves into its folder.
    stack.setRoots({builtin.dir()}, 1);
    REQUIRE(kept->save(sampleTool(5)));
    CHECK(exists(toolFile(user.dir(), fixedId(5))));
    stack.setRoots({builtin.dir(), user.dir()}, 1);
    CHECK(stack.tool(ray::ToolId{fixedId(5)}) != nullptr);
}
