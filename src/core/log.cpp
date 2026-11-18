// ray::log: a minimal, thread-safe logger with a replaceable sink (log_sink.h).
#include "log_sink.h"

#include <cstdio>
#include <mutex>
#include <string>

namespace ray {
namespace detail {
namespace {

std::mutex& sinkMutex() noexcept
{
    static std::mutex mutex;
    return mutex;
}

LogSink& currentSink() noexcept
{
    static LogSink sink;
    return sink;
}

const char* levelName(LogLevel level) noexcept
{
    switch (level) {
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Error:
        return "error";
    }
    return "?";
}

} // namespace

LogSink setLogSink(LogSink sink) noexcept
{
    const std::lock_guard<std::mutex> lock(sinkMutex());
    currentSink().swap(sink);
    return sink;
}

void writeLogLineToStderr(LogLevel level, std::string_view module, std::string_view message) noexcept
{
    try {
        std::string line;
        line.reserve(module.size() + message.size() + 16);
        line += '[';
        line += levelName(level);
        line += "] ";
        line += module;
        line += ": ";
        line += message;
        line += '\n';
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fflush(stderr);
    } catch (...) {
        // Out of memory while logging: drop the line.
    }
}

} // namespace detail

// The sink runs under the lock, so lines never interleave and a sink needs no locking of its own. A sink must
// not call ray::log or setLogSink itself.
void log(LogLevel level, std::string_view module, std::string_view message)
{
    try {
        const std::lock_guard<std::mutex> lock(detail::sinkMutex());
        const detail::LogSink& sink = detail::currentSink();
        if (sink) {
            sink(level, module, message);
        } else if (level != LogLevel::Debug) {
            detail::writeLogLineToStderr(level, module, message);
        }
    } catch (...) {
        // A failing sink must not take the caller down; the line is lost.
    }
}

} // namespace ray
