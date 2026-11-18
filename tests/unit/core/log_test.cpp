// ray::log and its replaceable sink.
#include <catch2/catch_test_macros.hpp>

#include <log_sink.h>

#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Line {
    ray::LogLevel level;
    std::string module;
    std::string message;
};

} // namespace

TEST_CASE("log goes to the installed sink", "[log]")
{
    std::vector<Line> lines;
    ray::detail::setLogSink([&lines](ray::LogLevel level, std::string_view module, std::string_view message) {
        lines.push_back({level, std::string(module), std::string(message)});
    });
    ray::log(ray::LogLevel::Warn, "core", "first");
    ray::log(ray::LogLevel::Debug, "cam", "second");
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].level == ray::LogLevel::Warn);
    CHECK(lines[0].module == "core");
    CHECK(lines[0].message == "first");
    CHECK(lines[1].level == ray::LogLevel::Debug);
    CHECK(lines[1].module == "cam");

    // Many threads at once: the sink is never entered concurrently, so the plain vector is safe.
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([] {
            for (int i = 0; i < 100; ++i) {
                ray::log(ray::LogLevel::Info, "test", "line");
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(lines.size() == 802);

    // Restoring the default sink returns the one that was installed.
    const ray::detail::LogSink previous = ray::detail::setLogSink({});
    CHECK(static_cast<bool>(previous));
}

TEST_CASE("a throwing sink does not throw out of log", "[log]")
{
    ray::detail::setLogSink(
        [](ray::LogLevel, std::string_view, std::string_view) { throw std::runtime_error("sink failed"); });
    CHECK_NOTHROW(ray::log(ray::LogLevel::Error, "core", "lost"));
    ray::detail::setLogSink({});
    // The default sink writes to stderr.
    CHECK_NOTHROW(ray::log(ray::LogLevel::Info, "core", "ray_core_tests: default sink line (expected in output)"));
}
