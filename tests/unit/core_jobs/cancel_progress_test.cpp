// CancelToken, CancelSource and Progress (task T-003).
#include <catch2/catch_test_macros.hpp>

#include <ray/core.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

struct Report {
    double fraction;
    std::string stage;
};

// Collects what a ProgressListener receives. The listener is never called concurrently, but the test
// thread reads while workers report, so the list is guarded.
class Recorder {
public:
    ray::ProgressListener listener()
    {
        return [this](double fraction, std::string_view stage) {
            std::lock_guard<std::mutex> lock(mutex_);
            reports_.push_back(Report{fraction, std::string(stage)});
        };
    }
    std::vector<Report> reports() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return reports_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Report> reports_;
};

} // namespace

TEST_CASE("a default CancelToken is never cancelled", "[jobs][cancel]")
{
    const ray::CancelToken token;
    CHECK_FALSE(token.cancelled());
    CHECK(token.check().has_value());
}

TEST_CASE("CancelSource cancels its tokens, and linked sources follow their parent", "[jobs][cancel]")
{
    ray::CancelSource parent;
    ray::CancelSource child(parent.token());
    const ray::CancelToken parentToken = parent.token();
    const ray::CancelToken childToken = child.token();
    CHECK_FALSE(childToken.cancelled());

    SECTION("cancelling the child leaves the parent alone")
    {
        child.cancel();
        CHECK(childToken.cancelled());
        CHECK(child.cancelled());
        CHECK_FALSE(parentToken.cancelled());
        const ray::Status status = childToken.check();
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ray::ErrorCode::Cancelled);
    }
    SECTION("cancelling the parent cancels the child")
    {
        parent.cancel();
        CHECK(parentToken.cancelled());
        CHECK(childToken.cancelled());
        CHECK(child.cancelled());
    }
    SECTION("a copy of a source shares its state")
    {
        ray::CancelSource copy = child;
        copy.cancel();
        CHECK(childToken.cancelled());
    }
}

TEST_CASE("cancel is seen by another thread", "[jobs][cancel]")
{
    ray::CancelSource source;
    std::atomic<bool> sawIt{false};
    std::thread watcher([token = source.token(), &sawIt] {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!token.cancelled() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        sawIt = token.cancelled();
    });
    source.cancel();
    watcher.join();
    CHECK(sawIt);
}

TEST_CASE("a default Progress and a default JobContext are no-op sinks", "[jobs][progress]")
{
    ray::JobContext ctx{};
    ctx.progress.set(0.5, "stage");
    ray::Progress child = ctx.progress.sub(0.5);
    child.set(1.0);
    child.setListener([](double, std::string_view) { FAIL("a no-op sink has no listener"); });
    child.set(1.0);
    CHECK_FALSE(ctx.cancel.cancelled());
    CHECK(ctx.cancel.check().has_value());
}

TEST_CASE("sub ranges map into the parent range in order", "[jobs][progress]")
{
    Recorder recorder;
    ray::Progress root(recorder.listener());
    ray::Progress first = root.sub(0.25);
    ray::Progress second = root.sub(0.75);
    ray::Progress firstHalf = second.sub(0.5);

    first.set(1.0, "first"); // the very first report is never held back
    std::this_thread::sleep_for(40ms);
    firstHalf.set(0.5, "second"); // 0.25 + 0.75 * 0.5 * 0.5
    std::this_thread::sleep_for(40ms);
    second.set(1.0);

    const std::vector<Report> reports = recorder.reports();
    REQUIRE(reports.size() == 3);
    CHECK(reports[0].fraction == 0.25);
    CHECK(reports[0].stage == "first");
    CHECK(reports[1].fraction == 0.25 + 0.75 * 0.25);
    CHECK(reports[1].stage == "second");
    CHECK(reports[2].fraction == 1.0);
}

TEST_CASE("sub ranges never exceed the parent range", "[jobs][progress]")
{
    Recorder recorder;
    ray::Progress root(recorder.listener());
    ray::Progress a = root.sub(0.75);
    ray::Progress b = root.sub(0.75); // only 0.25 is left
    b.set(0.0);
    std::this_thread::sleep_for(40ms);
    b.set(1.0); // the end of the range: the final report
    const std::vector<Report> reports = recorder.reports();
    REQUIRE(reports.size() == 2);
    CHECK(reports[0].fraction == 0.75);
    CHECK(reports[1].fraction == 1.0);
}

TEST_CASE("the reported fraction never decreases", "[jobs][progress]")
{
    Recorder recorder;
    ray::Progress root(recorder.listener());
    root.set(0.5);
    std::this_thread::sleep_for(40ms);
    root.set(0.25); // ignored: lower than what was reported
    std::this_thread::sleep_for(40ms);
    root.set(0.75);
    const std::vector<Report> reports = recorder.reports();
    REQUIRE(reports.size() == 2);
    CHECK(reports[0].fraction == 0.5);
    CHECK(reports[1].fraction == 0.75);
}

TEST_CASE("progress is rate-limited to 30 Hz and the final 1.0 is always delivered", "[jobs][progress]")
{
    for (int round = 0; round < 3; ++round) {
        std::atomic<int> calls{0};
        std::mutex mutex;
        std::vector<double> fractions;
        ray::Progress root([&](double fraction, std::string_view) {
            ++calls;
            std::lock_guard<std::mutex> lock(mutex);
            fractions.push_back(fraction);
        });

        constexpr int kThreads = 4;
        constexpr int kSteps = 100'000;
        const auto start = std::chrono::steady_clock::now();
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&root, t] {
                for (int i = 0; i < kSteps; ++i) {
                    root.set(0.999 * (i + t) / (kSteps + kThreads), i % 25'000 == 0 ? "stage" : "");
                    if (i % 1000 == 0) {
                        std::this_thread::sleep_for(100us);
                    }
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        root.set(1.0); // final: delivered no matter how soon after the last report
        root.set(1.0); // delivered once only
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        std::lock_guard<std::mutex> lock(mutex);
        INFO("round " << round << ": " << fractions.size() << " reports in " << seconds << " s");
        REQUIRE(!fractions.empty());
        CHECK(fractions.back() == 1.0);
        CHECK(static_cast<int>(fractions.size()) == calls.load());
        // At most one report per 1/30 s, plus the first one and the final one.
        CHECK(static_cast<double>(fractions.size()) <= seconds * ray::Progress::kMaxRateHz + 2.0);
        for (std::size_t i = 1; i < fractions.size(); ++i) {
            CHECK(fractions[i] >= fractions[i - 1]);
        }
        std::size_t finals = 0;
        for (const double fraction : fractions) {
            finals += fraction == 1.0 ? 1 : 0;
        }
        CHECK(finals == 1);
    }
}

TEST_CASE("setListener replaces the listener of the whole tree", "[jobs][progress]")
{
    Recorder before;
    Recorder after;
    ray::Progress root(before.listener());
    ray::Progress child = root.sub(1.0);
    child.set(0.5);
    child.setListener(after.listener());
    child.set(1.0);
    REQUIRE(before.reports().size() == 1);
    REQUIRE(after.reports().size() == 1);
    CHECK(after.reports()[0].fraction == 1.0);
}
