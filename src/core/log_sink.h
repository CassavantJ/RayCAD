// Private to src/core and its tests: where ray::log() sends its lines.
#pragma once

#include <ray/core.h>

#include <functional>
#include <string_view>

namespace ray::detail {

using LogSink = std::function<void(LogLevel, std::string_view module, std::string_view message)>;

// Replaces the sink and returns the previous one. An empty function restores the default sink, which writes
// Info and above to stderr as "[level] module: message". The sink may be called from any thread, but never
// from two threads at once (it runs under a lock), so it must not call ray::log or setLogSink itself.
// Exceptions thrown by the sink are swallowed and the line is lost.
LogSink setLogSink(LogSink sink) noexcept;

// Writes one line in the default format to stderr. The default sink uses it for Info and above.
void writeLogLineToStderr(LogLevel level, std::string_view module, std::string_view message) noexcept;

} // namespace ray::detail
