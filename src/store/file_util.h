// Small helpers shared by the store sources: UTF-8 paths, errors, clocks. Private to src/store.
#pragma once
#include <ray/core.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace ray::store::detail {

// Every path crossing the store's interface is UTF-8. std::filesystem::path(std::string) would read the bytes in
// the ANSI code page on Windows, so conversions always go through char8_t.
std::filesystem::path toFsPath(std::string_view utf8);
std::string fromFsPath(const std::filesystem::path& path);

bool pathExists(const std::string& utf8) noexcept;
// Removes a file if it is there; errors are ignored (best-effort cleanup).
void removeQuietly(const std::string& utf8) noexcept;

// Wall-clock time in milliseconds since 1970 (commit times and the lock file's informational fields only).
std::int64_t unixNowMs() noexcept;
// 32 random hex digits, for temporary and renamed file names.
std::string randomName();

Error makeError(ErrorCode code, std::string message, std::string context = {});
inline tl::unexpected<Error> fail(ErrorCode code, std::string message, std::string context = {})
{
    return tl::unexpected<Error>(makeError(code, std::move(message), std::move(context)));
}
// Adds a context line (innermost first) to an error and passes it on.
inline tl::unexpected<Error> rethrow(Error error, std::string context)
{
    error.context.push_back(std::move(context));
    return tl::unexpected<Error>(std::move(error));
}

} // namespace ray::store::detail
