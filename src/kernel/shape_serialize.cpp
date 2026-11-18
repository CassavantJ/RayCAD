// Canonical binary BREP serialisation of a Shape (task T-004).
//
// FORMAT, version 1. Integers in the envelope are little-endian.
//   offset 0       7 bytes   magic "RAYBREP"
//   offset 7       1 byte    format version: 1
//   offset 8       8 bytes   payload length n, unsigned
//   offset 16      n bytes   payload: an OpenCASCADE BinTools shape stream, BinTools format V4
//   offset 16 + n  32 bytes  BLAKE3-256 of bytes [0, 16 + n)
//
// The payload has the layout BinTools_ShapeSet::Write produces (header line, locations, geometry, TShapes,
// then a reference to the root), so OCCT's own BinTools::Read can read it, but it is written here so that the
// bytes are canonical, i.e. a function of the shape's geometry and topology only:
//   - No mesh data: triangulations, 3D polygons and polygons on triangulation are never written. (A face
//     without a surface keeps its triangulation: that is its geometry. BinTools does this itself.)
//   - The three bookkeeping flags OCCT stores on every TShape are written as fixed values: Free only on the
//     root, Modified set, Checked clear. OCCT changes these in place on TShapes that several shapes share
//     (BRepMesh sets Modified, TopoDS_Builder::Add clears Free on what it adds, BRepTools::Update sets
//     Checked), so writing them as found would make the bytes depend on what happened to the shape after it
//     was built. The geometric flags (Orientable, Closed, Infinite, Convex) are written as found.
//   - The stream uses the classic locale. BinTools writes numbers in a fixed binary encoding and orders
//     everything by first use in a walk of the shape (sub-shapes before the shapes that contain them), so no
//     pointer, address, hash order, time stamp or thread count reaches the bytes.
// Hence serialize(deserialize(b)) == b for every b that serialize() produced, and contentHash() is BLAKE3 of
// these bytes. Bytes may still differ across operating systems where OCCT computed the geometry differently
// in the last bits (PLAN.md section 8).
//
// deserialize() checks the envelope (size, magic, version, length, checksum) before OCCT sees any byte, so
// truncated or damaged data is rejected with FormatError (FormatTooNew for a newer version). The BinTools
// reader trusts the counts and indices inside a payload whose checksum is right; this file checks the header
// line, the root reference and that the payload is consumed exactly, and the reader runs under the kernel's
// exception guard, but a payload crafted with a matching checksum is not otherwise validated.
#include "shape_impl.h"

#include <BinTools.hxx>
#include <BinTools_FormatVersion.hxx>
#include <BinTools_LocationSet.hxx>
#include <BinTools_ShapeSet.hxx>
#include <TopoDS_Iterator.hxx>

#include <algorithm>
#include <array>
#include <cstring>
#include <ios>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace ray::kernel {

namespace {

constexpr std::array<char, 7> kMagic{'R', 'A', 'Y', 'B', 'R', 'E', 'P'};
constexpr std::uint8_t kFormatVersion = 1;
constexpr std::size_t kHeaderSize = 16;
constexpr std::size_t kChecksumSize = 32;
constexpr int kBinToolsFormat = BinTools_FormatVersion_VERSION_4;
// BinTools_ShapeSetBase::THE_ASCII_VERSIONS[4]. Spelled out because that array is not exported from the DLL;
// the round-trip tests fail if it ever stops matching.
constexpr std::string_view kBinToolsVersionLine = "Open CASCADE Topology V4, (c) Open Cascade";

std::string writePayload(const TopoDS_Shape& shape)
{
    std::ostringstream os(std::ios::out | std::ios::binary);
    os.imbue(std::locale::classic());

    BinTools_ShapeSet set;
    set.SetWithTriangles(false);
    set.SetWithNormals(false);
    set.SetFormatNb(kBinToolsFormat);
    set.Add(shape);

    os << '\n' << kBinToolsVersionLine << '\n';
    set.Locations().Write(os);
    set.WriteGeometry(os);

    const int count = set.NbShapes();
    os << "\nTShapes " << count << '\n';
    for (int i = 1; i <= count; ++i) {
        const TopoDS_Shape& sub = set.Shape(i);
        os.put(static_cast<char>(sub.ShapeType()));
        set.WriteShape(sub, os);
        BinTools::PutBool(os, i == count); // Free: canonical, see the file comment
        BinTools::PutBool(os, true);       // Modified: canonical
        BinTools::PutBool(os, false);      // Checked: canonical
        BinTools::PutBool(os, sub.Orientable());
        BinTools::PutBool(os, sub.Closed());
        BinTools::PutBool(os, sub.Infinite());
        BinTools::PutBool(os, sub.Convex());
        for (TopoDS_Iterator it(sub, false, false); it.More(); it.Next())
            set.Write(it.Value(), os);
        set.Write(TopoDS_Shape(), os); // end of the sub-shape list
    }
    set.Write(shape, os); // the root: orientation, TShape index, location index

    if (!os)
        throw std::runtime_error("writing the BREP stream failed");
    return os.str();
}

Result<Shape> readPayload(std::span<const std::byte> payload, std::string_view where)
{
    auto damaged = [&](const char* what) {
        return tl::unexpected(
            detail::makeError(ErrorCode::FormatError, std::string("The BREP data is damaged: ") + what + ".", where));
    };

    std::string text(reinterpret_cast<const char*>(payload.data()), payload.size());
    const std::string expectedHeader = '\n' + std::string(kBinToolsVersionLine) + '\n';
    if (text.compare(0, expectedHeader.size(), expectedHeader) != 0)
        return damaged("unexpected BinTools header");

    std::istringstream is(text, std::ios::in | std::ios::binary);
    is.imbue(std::locale::classic());

    BinTools_ShapeSet set;
    set.SetWithTriangles(true); // reads a surface-less face's triangulation back
    set.Read(is);
    const int count = set.NbShapes();
    if (is.fail() || count < 1)
        return damaged("the shape table could not be read");

    const int orientation = is.get();
    int index = 0;
    int location = 0;
    BinTools::GetInteger(is, index);
    BinTools::GetInteger(is, location);
    if (is.fail() || orientation < TopAbs_FORWARD || orientation > TopAbs_EXTERNAL || index < 1 || index > count ||
        location < 0 || location > set.Locations().NbLocations())
        return damaged("bad root reference");
    if (is.peek() != std::istringstream::traits_type::eof())
        return damaged("unexpected bytes after the shape");

    TopoDS_Shape root = set.Shape(count - index + 1);
    root.Orientation(static_cast<TopAbs_Orientation>(orientation));
    root.Location(set.Locations().Location(location), false);
    return detail::wrap(root);
}

void putU64(std::byte* out, std::uint64_t value)
{
    for (int i = 0; i < 8; ++i)
        out[i] = static_cast<std::byte>((value >> (8 * i)) & 0xFFu);
}

std::uint64_t getU64(const std::byte* in)
{
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
        value |= static_cast<std::uint64_t>(in[i]) << (8 * i);
    return value;
}

} // namespace

namespace detail {

std::vector<std::byte> sealPayload(std::span<const std::byte> payload)
{
    std::vector<std::byte> bytes(kHeaderSize + payload.size() + kChecksumSize);
    std::memcpy(bytes.data(), kMagic.data(), kMagic.size());
    bytes[7] = static_cast<std::byte>(kFormatVersion);
    putU64(bytes.data() + 8, payload.size());
    if (!payload.empty())
        std::memcpy(bytes.data() + kHeaderSize, payload.data(), payload.size());
    const Hash256 checksum = blake3(std::span<const std::byte>(bytes.data(), kHeaderSize + payload.size()));
    std::memcpy(bytes.data() + kHeaderSize + payload.size(), checksum.bytes.data(), kChecksumSize);
    return bytes;
}

std::span<const std::byte> payloadOf(std::span<const std::byte> bytes) noexcept
{
    if (bytes.size() < kHeaderSize + kChecksumSize)
        return {};
    const std::uint64_t length = getU64(bytes.data() + 8);
    if (length != bytes.size() - kHeaderSize - kChecksumSize)
        return {};
    return bytes.subspan(kHeaderSize, static_cast<std::size_t>(length));
}

std::vector<std::byte> serializeOcct(const TopoDS_Shape& shape) noexcept
{
    if (shape.IsNull())
        return {};
    struct Context {
        const TopoDS_Shape* shape;
        std::vector<std::byte>* out;
    };
    std::vector<std::byte> out;
    Context context{&shape, &out};
    std::string message;
    const bool ok = runGuarded(
        [](void* raw) {
            auto& ctx = *static_cast<Context*>(raw);
            const std::string payload = writePayload(*ctx.shape);
            *ctx.out = sealPayload(std::as_bytes(std::span<const char>(payload.data(), payload.size())));
        },
        &context, message);
    if (!ok)
        out.clear();
    return out;
}

} // namespace detail

std::vector<std::byte> serialize(const Shape& shape)
{
    return detail::serializeOcct(detail::occt(shape));
}

Result<Shape> deserialize(std::span<const std::byte> bytes)
{
    constexpr std::string_view where = "kernel::deserialize";
    auto formatError = [&](const char* what) {
        return tl::unexpected(detail::makeError(ErrorCode::FormatError, what, where));
    };

    if (bytes.size() < kHeaderSize + kChecksumSize)
        return formatError("The BREP data is truncated.");
    if (std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0)
        return formatError("The data is not RayCAD BREP data.");
    const auto version = static_cast<std::uint8_t>(bytes[7]);
    if (version == 0)
        return formatError("The BREP data has an invalid format version.");
    if (version > kFormatVersion)
        return tl::unexpected(detail::makeError(ErrorCode::FormatTooNew,
                                                "The BREP data was written by a newer version of RayCAD.", where));
    const std::span<const std::byte> payload = detail::payloadOf(bytes);
    if (payload.empty())
        return formatError("The BREP data is truncated or has extra bytes at the end.");
    const Hash256 checksum = detail::blake3(bytes.first(kHeaderSize + payload.size()));
    if (!std::equal(checksum.bytes.begin(), checksum.bytes.end(),
                    reinterpret_cast<const std::uint8_t*>(bytes.data() + kHeaderSize + payload.size())))
        return formatError("The BREP data is damaged (checksum mismatch).");

    return detail::guarded(where, [&]() -> Result<Shape> { return readPayload(payload, where); });
}

} // namespace ray::kernel
