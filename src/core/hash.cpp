// ray::Hasher: BLAKE3-256 over a stream of typed fields.
//
// Encoding of each field (all integers little-endian, independent of the host):
//   bytes(b)  the bytes as they are, with no length;
//   str(s)    the length as 8 bytes, then the bytes of s, so consecutive strings cannot run into each other;
//   f64(v)    the 8 bytes of the IEEE-754 bit pattern, with -0.0 replaced by +0.0 (NaN payloads are kept);
//   i64(v)    the 8 bytes of the two's complement value.
// finish() does not change the state: more fields may follow, and calling it again gives the hash of
// everything so far. A moved-from Hasher starts again from empty input.
// Running out of memory in the constructor terminates the program (it is noexcept); nothing throws.
#include <ray/core.h>

#include <bit>
#include <blake3.h>

namespace ray {

struct Hasher::Impl {
    blake3_hasher state;
};

namespace {

void putLe64(std::uint8_t (&out)[8], std::uint64_t value) noexcept
{
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

} // namespace

Hasher::Hasher() noexcept : impl_(std::make_unique<Impl>())
{
    blake3_hasher_init(&impl_->state);
}

Hasher::~Hasher() = default;
Hasher::Hasher(Hasher&&) noexcept = default;
Hasher& Hasher::operator=(Hasher&&) noexcept = default;

Hasher& Hasher::bytes(std::span<const std::byte> data)
{
    if (!impl_) {
        *this = Hasher();
    }
    blake3_hasher_update(&impl_->state, data.data(), data.size());
    return *this;
}

Hasher& Hasher::str(std::string_view text)
{
    std::uint8_t length[8];
    putLe64(length, static_cast<std::uint64_t>(text.size()));
    bytes(std::as_bytes(std::span<const std::uint8_t>(length)));
    return bytes(std::as_bytes(std::span<const char>(text.data(), text.size())));
}

Hasher& Hasher::f64(double value)
{
    if (value == 0.0) {
        value = 0.0; // turns -0.0 into +0.0
    }
    std::uint8_t out[8];
    putLe64(out, std::bit_cast<std::uint64_t>(value));
    return bytes(std::as_bytes(std::span<const std::uint8_t>(out)));
}

Hasher& Hasher::i64(std::int64_t value)
{
    std::uint8_t out[8];
    putLe64(out, static_cast<std::uint64_t>(value));
    return bytes(std::as_bytes(std::span<const std::uint8_t>(out)));
}

Hash256 Hasher::finish()
{
    if (!impl_) {
        *this = Hasher();
    }
    Hash256 hash;
    blake3_hasher_finalize(&impl_->state, hash.bytes.data(), hash.bytes.size());
    return hash;
}

} // namespace ray
