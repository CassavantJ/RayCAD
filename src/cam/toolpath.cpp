// Toolpath::serialize, deserialize and hash: the RCTP golden-file format, version 2 (cam.h "Move: the RCTP record",
// PLAN.md 3.6). Written field by field in little-endian order, so the bytes do not depend on the platform's byte order
// or on how the compiler lays out Move (which cam.h nevertheless pins to the same 112 bytes).
#include <ray/cam.h>

#include <algorithm>
#include <array>
#include <string>

namespace ray::cam {
namespace {

constexpr std::array<std::byte, 4> kMagic = {std::byte{'R'}, std::byte{'C'}, std::byte{'T'}, std::byte{'P'}};
constexpr std::size_t kHeaderBytes = 88;
constexpr std::size_t kMoveBytes = 112;
constexpr std::uint8_t kMoveKindCount = static_cast<std::uint8_t>(MoveKind::CycleCancel) + 1;
constexpr std::uint8_t kMoveRoleCount = static_cast<std::uint8_t>(MoveRole::Approach) + 1;
constexpr std::uint8_t kArcPlaneCount = static_cast<std::uint8_t>(ArcPlane::YZ) + 1;
constexpr std::uint8_t kCycleReturnCount = static_cast<std::uint8_t>(CycleReturn::Initial) + 1;

class Writer {
public:
    explicit Writer(std::vector<std::byte>& out) : out_(out) {}
    void u8(std::uint8_t v) { out_.push_back(static_cast<std::byte>(v)); }
    void unsignedLe(std::uint64_t v, int bytes)
    {
        for (int i = 0; i < bytes; ++i) {
            out_.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
        }
    }
    void u16(std::uint16_t v) { unsignedLe(v, 2); }
    void u32(std::uint32_t v) { unsignedLe(v, 4); }
    void i32(std::int32_t v) { unsignedLe(static_cast<std::uint32_t>(v), 4); }
    void u64(std::uint64_t v) { unsignedLe(v, 8); }
    void i64(std::int64_t v) { unsignedLe(static_cast<std::uint64_t>(v), 8); }
    template <std::size_t N>
    void raw(const std::array<std::uint8_t, N>& bytes)
    {
        for (std::uint8_t b : bytes) {
            u8(b);
        }
    }

private:
    std::vector<std::byte>& out_;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> in) : in_(in) {}
    std::uint8_t u8() { return static_cast<std::uint8_t>(in_[pos_++]); }
    std::uint64_t unsignedLe(int bytes)
    {
        std::uint64_t v = 0;
        for (int i = 0; i < bytes; ++i) {
            v |= static_cast<std::uint64_t>(u8()) << (8 * i);
        }
        return v;
    }
    std::uint16_t u16() { return static_cast<std::uint16_t>(unsignedLe(2)); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(unsignedLe(4)); }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    std::uint64_t u64() { return unsignedLe(8); }
    std::int64_t i64() { return static_cast<std::int64_t>(u64()); }
    template <std::size_t N>
    void raw(std::array<std::uint8_t, N>& bytes)
    {
        for (std::uint8_t& b : bytes) {
            b = u8();
        }
    }

private:
    std::span<const std::byte> in_;
    std::size_t pos_ = 0;
};

void writeMove(Writer& w, const Move& m)
{
    w.u8(static_cast<std::uint8_t>(m.kind));
    w.u8(static_cast<std::uint8_t>(m.role));
    w.u8(static_cast<std::uint8_t>(m.plane));
    w.u8(m.compensation);
    w.u8(static_cast<std::uint8_t>(m.cycleReturn));
    w.u8(m.cycleLeftHand);
    w.u16(0); // pad0
    w.i32(m.feedMicronPerMin);
    w.i32(m.cycleDwellMs);
    w.i32(m.threadPitchNm);
    w.u32(0); // pad1
    for (Nm v : {m.x, m.y, m.z, m.i, m.j, m.k, m.cycleRetract, m.cycleTop, m.cycleClearance, m.cyclePeck,
                 m.cyclePeckClearance}) {
        w.i64(v);
    }
}

tl::unexpected<Error> formatError(std::string message)
{
    return tl::unexpected(Error{ErrorCode::FormatError, "toolpath: " + std::move(message), {}});
}

} // namespace

std::vector<std::byte> Toolpath::serialize() const
{
    std::vector<std::byte> out;
    out.reserve(kHeaderBytes + kMoveBytes * moves.size());
    out.insert(out.end(), kMagic.begin(), kMagic.end());
    Writer w(out);
    w.u32(kToolpathFormatVersion);
    w.raw(operation.value.bytes);
    w.raw(inputHash.bytes);
    w.u64(static_cast<std::uint64_t>(moves.size()));
    w.i64(startX);
    w.i64(startY);
    w.i64(startZ);
    for (const Move& m : moves) {
        writeMove(w, m);
    }
    return out;
}

Result<Toolpath> Toolpath::deserialize(std::span<const std::byte> bytes)
{
    if (bytes.size() < kHeaderBytes || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        return formatError("not an RCTP toolpath");
    }
    Reader r(bytes.subspan(kMagic.size()));
    const std::uint32_t version = r.u32();
    if (version > kToolpathFormatVersion) {
        return tl::unexpected(Error{ErrorCode::FormatTooNew,
                                    "toolpath: format version " + std::to_string(version) +
                                        " was written by a newer RayCAD (this one reads version " +
                                        std::to_string(kToolpathFormatVersion) + ")",
                                    {}});
    }
    if (version != kToolpathFormatVersion) {
        return formatError("format version " + std::to_string(version) + " is not readable (only version " +
                           std::to_string(kToolpathFormatVersion) + ")");
    }
    Toolpath tp{};
    r.raw(tp.operation.value.bytes);
    r.raw(tp.inputHash.bytes);
    const std::uint64_t count = r.u64();
    if (count > (bytes.size() - kHeaderBytes) / kMoveBytes || bytes.size() != kHeaderBytes + count * kMoveBytes) {
        return formatError("the size does not match the move count");
    }
    tp.startX = r.i64();
    tp.startY = r.i64();
    tp.startZ = r.i64();
    tp.moves.resize(static_cast<std::size_t>(count));
    for (std::size_t n = 0; n < tp.moves.size(); ++n) {
        Move& m = tp.moves[n];
        const std::uint8_t kind = r.u8(), role = r.u8(), plane = r.u8(), comp = r.u8(), ret = r.u8(), left = r.u8();
        const std::uint16_t pad0 = r.u16();
        m.feedMicronPerMin = r.i32();
        m.cycleDwellMs = r.i32();
        m.threadPitchNm = r.i32();
        const std::uint32_t pad1 = r.u32();
        for (Nm* v : {&m.x, &m.y, &m.z, &m.i, &m.j, &m.k, &m.cycleRetract, &m.cycleTop, &m.cycleClearance, &m.cyclePeck,
                      &m.cyclePeckClearance}) {
            *v = r.i64();
        }
        if (kind >= kMoveKindCount || role >= kMoveRoleCount || plane >= kArcPlaneCount || comp > 2 ||
            ret >= kCycleReturnCount || left > 1 || pad0 != 0 || pad1 != 0) {
            return formatError("move " + std::to_string(n) + " has a value outside its range");
        }
        m.kind = static_cast<MoveKind>(kind);
        m.role = static_cast<MoveRole>(role);
        m.plane = static_cast<ArcPlane>(plane);
        m.compensation = comp;
        m.cycleReturn = static_cast<CycleReturn>(ret);
        m.cycleLeftHand = left;
    }
    return tp;
}

Hash256 Toolpath::hash() const
{
    Hasher h;
    h.bytes(serialize());
    return h.finish();
}

} // namespace ray::cam
