// Small helpers shared by the store sources.
#include "file_util.h"

#include <chrono>
#include <system_error>

namespace ray::store::detail {

std::filesystem::path toFsPath(std::string_view utf8)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string fromFsPath(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

bool pathExists(const std::string& utf8) noexcept
{
    try {
        std::error_code ec;
        return std::filesystem::exists(toFsPath(utf8), ec);
    } catch (...) {
        return false;
    }
}

void removeQuietly(const std::string& utf8) noexcept
{
    try {
        std::error_code ec;
        std::filesystem::remove(toFsPath(utf8), ec);
    } catch (...) {
    }
}

std::int64_t unixNowMs() noexcept
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string randomName()
{
    const Uuid id = Uuid::generate();
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (const std::uint8_t byte : id.bytes) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 15]);
    }
    return out;
}

Error makeError(ErrorCode code, std::string message, std::string context)
{
    Error error{code, std::move(message), {}};
    if (!context.empty()) {
        error.context.push_back(std::move(context));
    }
    return error;
}

} // namespace ray::store::detail
