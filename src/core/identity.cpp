// Uuid (identity) and Hash256 text forms.
#include <ray/core.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <random>

namespace ray {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

int hexValue(char c) noexcept
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::uint64_t splitmix64(std::uint64_t& state) noexcept
{
    std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

// Used only when the operating system's random source is unavailable: mixes the clock, a process-wide counter
// and an address. Not cryptographic, but distinct ids per call and per process are all a Uuid needs.
void fallbackRandom(std::array<std::uint8_t, 16>& out) noexcept
{
    static std::atomic<std::uint64_t> counter{0};
    std::uint64_t state = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    state ^= static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()) << 1;
    state ^= counter.fetch_add(1, std::memory_order_relaxed) * 0xd1b54a32d192ed03ULL;
    state ^= static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&counter));
    const std::uint64_t a = splitmix64(state);
    const std::uint64_t b = splitmix64(state);
    std::memcpy(out.data(), &a, 8);
    std::memcpy(out.data() + 8, &b, 8);
}

} // namespace

Uuid Uuid::generate()
{
    Uuid id;
    try {
        std::random_device device;
        for (std::size_t i = 0; i < id.bytes.size(); i += 4) {
            const std::uint32_t word = device();
            std::memcpy(id.bytes.data() + i, &word, 4);
        }
    } catch (...) {
        fallbackRandom(id.bytes);
    }
    id.bytes[6] = static_cast<std::uint8_t>((id.bytes[6] & 0x0F) | 0x40); // version 4
    id.bytes[8] = static_cast<std::uint8_t>((id.bytes[8] & 0x3F) | 0x80); // RFC 4122 variant
    return id;
}

// Accepts exactly the canonical form: 36 characters, hyphens at offsets 8, 13, 18 and 23, hex digits
// (either case) everywhere else. Anything else returns the nil Uuid (all zero bytes).
Uuid Uuid::fromString(std::string_view text)
{
    Uuid id;
    if (text.size() != 36) {
        return {};
    }
    std::size_t byte = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') {
                return {};
            }
            ++i;
            continue;
        }
        const int hi = hexValue(text[i]);
        const int lo = i + 1 < text.size() ? hexValue(text[i + 1]) : -1;
        if (hi < 0 || lo < 0 || byte >= id.bytes.size()) {
            return {};
        }
        id.bytes[byte++] = static_cast<std::uint8_t>(hi * 16 + lo);
        i += 2;
    }
    return byte == id.bytes.size() ? id : Uuid{};
}

std::string Uuid::toString() const
{
    try {
        std::string out;
        out.reserve(36);
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            if (i == 4 || i == 6 || i == 8 || i == 10) {
                out.push_back('-');
            }
            out.push_back(kHexDigits[bytes[i] >> 4]);
            out.push_back(kHexDigits[bytes[i] & 0x0F]);
        }
        return out;
    } catch (...) {
        return {};
    }
}

std::string Hash256::hex() const
{
    try {
        std::string out;
        out.reserve(64);
        for (const std::uint8_t b : bytes) {
            out.push_back(kHexDigits[b >> 4]);
            out.push_back(kHexDigits[b & 0x0F]);
        }
        return out;
    } catch (...) {
        return {};
    }
}

} // namespace ray
