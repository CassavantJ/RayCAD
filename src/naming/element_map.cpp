// ElementMap, TopoName ids and families, and the element-map file format (task T-007).
//
// Serialised form, version 1. All integers little-endian; Hash256 as its 32 bytes; Uuid as its 16 bytes.
//   "RCEM"  u32 version = 1
//   u32 elementCount, then per element in SubId order:
//       u8 kind, u32 index,
//       fingerprint: u8 surface, i64 qArea, qCx, qCy, qCz, qRadius, qAxisX, qAxisY, qAxisZ, u16 neighbourCount
//       u32 nameCount, nameCount x Hash256 (primary name first)
//   u32 recordCount, then per record in id order:
//       Hash256 id, u8 kind, Uuid origin, u32 roleLength, role bytes,
//       u32 parentCount, Hash256 x parentCount, u32 discriminatorCount, Hash256 x discriminatorCount,
//       u32 ordinal, u32 siblingCount, Hash256 family
//   version 2 only (T-081b round 4; written only when the map holds a placement, so a map without one is the same
//   version-1 byte string as before): u32 placementCount, then per placement in feature order:
//       Uuid feature, 12 x i64 (FeaturePlacement::q)
//   Hash256 checksum: BLAKE3 of every byte before it.
// The byte string is a function of the table alone, so equal maps serialise to equal bytes. deserialize()
// rejects anything that does not follow this layout or the rules of detail::finishMap. The lineage propagate() keeps in
// memory for pick-time evidence (ElementMap::Impl::ancestors and bodyMoves, T-081b round 9) is not part of the table:
// it is not written, and a map read back has none.
#include "naming_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace ray::naming {

namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'R', 'C', 'E', 'M'};
constexpr std::uint32_t kVersion = 2; // the newest this build reads and writes
constexpr std::uint32_t kVersionNoPlacements = 1;
constexpr std::size_t kPlacementBytes = 16 + 12 * 8;
constexpr std::size_t kMaxRoleLength = 4096;
// Smallest possible encoded sizes, to reject absurd counts before allocating.
constexpr std::size_t kMinElementBytes = 1 + 4 + 1 + 8 * 8 + 2 + 4 + 32;
constexpr std::size_t kMinRecordBytes = 32 + 1 + 16 + 4 + 4 + 4 + 4 + 4 + 32;

Error makeError(ErrorCode code, std::string message, std::string_view where)
{
    Error error{code, std::move(message), {}};
    error.context.emplace_back(where);
    return error;
}

bool validKind(kernel::SubKind kind) noexcept
{
    return static_cast<std::uint8_t>(kind) <= static_cast<std::uint8_t>(kernel::SubKind::Solid);
}

bool validSurface(std::uint8_t value) noexcept
{
    return value <= static_cast<std::uint8_t>(kernel::SurfaceType::Other);
}

void hashUuid(Hasher& hasher, const Uuid& uuid)
{
    hasher.bytes(std::as_bytes(std::span<const std::uint8_t>(uuid.bytes)));
}

void hashList(Hasher& hasher, std::span<const Hash256> list)
{
    hasher.i64(static_cast<std::int64_t>(list.size()));
    for (const Hash256& h : list)
        hasher.bytes(std::as_bytes(std::span<const std::uint8_t>(h.bytes)));
}

// ---------- writing ----------
class Writer {
public:
    void u8(std::uint8_t v) { out_.push_back(static_cast<std::byte>(v)); }
    void u16(std::uint16_t v) { le(v, 2); }
    void u32(std::uint32_t v) { le(v, 4); }
    void i64(std::int64_t v) { le(static_cast<std::uint64_t>(v), 8); }
    void raw(std::span<const std::uint8_t> bytes)
    {
        for (const std::uint8_t b : bytes)
            out_.push_back(static_cast<std::byte>(b));
    }
    void hash(const Hash256& h) { raw(h.bytes); }
    void hashes(std::span<const Hash256> list)
    {
        u32(static_cast<std::uint32_t>(list.size()));
        for (const Hash256& h : list)
            hash(h);
    }
    std::vector<std::byte>& bytes() { return out_; }

private:
    void le(std::uint64_t v, int count)
    {
        for (int i = 0; i < count; ++i)
            out_.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
    }
    std::vector<std::byte> out_;
};

// ---------- reading ----------
class Reader {
public:
    explicit Reader(std::span<const std::byte> in) : in_(in) {}
    bool ok() const { return ok_; }
    std::size_t remaining() const { return ok_ ? in_.size() - pos_ : 0; }
    bool atEnd() const { return ok_ && pos_ == in_.size(); }
    void fail() { ok_ = false; }

    std::uint8_t u8() { return static_cast<std::uint8_t>(le(1)); }
    std::uint16_t u16() { return static_cast<std::uint16_t>(le(2)); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(le(4)); }
    std::int64_t i64() { return static_cast<std::int64_t>(le(8)); }
    Hash256 hash()
    {
        Hash256 h;
        if (take(32))
            std::memcpy(h.bytes.data(), in_.data() + pos_ - 32, 32);
        return h;
    }
    // A count followed by `count` items of at least `minItemBytes` each.
    std::uint32_t count(std::size_t minItemBytes)
    {
        const std::uint32_t n = u32();
        if (ok_ && static_cast<std::uint64_t>(n) * minItemBytes > remaining())
            ok_ = false;
        return ok_ ? n : 0;
    }
    std::vector<Hash256> hashes()
    {
        std::vector<Hash256> out(count(32));
        for (Hash256& h : out)
            h = hash();
        return out;
    }
    std::string text(std::size_t maxLength)
    {
        const std::uint32_t n = u32();
        if (!ok_ || n > maxLength || n > remaining()) {
            ok_ = false;
            return {};
        }
        std::string out(reinterpret_cast<const char*>(in_.data() + pos_), n);
        pos_ += n;
        return out;
    }
    void raw(std::span<std::uint8_t> out)
    {
        if (take(out.size()))
            std::memcpy(out.data(), in_.data() + pos_ - out.size(), out.size());
    }

private:
    bool take(std::size_t n)
    {
        if (!ok_ || n > in_.size() - pos_) {
            ok_ = false;
            return false;
        }
        pos_ += n;
        return true;
    }
    std::uint64_t le(int n)
    {
        if (!take(static_cast<std::size_t>(n)))
            return 0;
        std::uint64_t v = 0;
        for (int i = 0; i < n; ++i)
            v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(in_[pos_ - n + i])) << (8 * i);
        return v;
    }
    std::span<const std::byte> in_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

Hash256 checksum(std::span<const std::byte> bytes)
{
    Hasher hasher;
    hasher.bytes(bytes);
    return hasher.finish();
}

const ElementMap::Impl::Element* findElement(const ElementMap::Impl& impl, kernel::SubId sub)
{
    const auto it = std::lower_bound(impl.elements.begin(), impl.elements.end(), sub,
                                     [](const ElementMap::Impl::Element& e, kernel::SubId s) { return e.sub < s; });
    return it != impl.elements.end() && it->sub == sub ? &*it : nullptr;
}

const TopoName* findRecord(const ElementMap::Impl& impl, const Hash256& id)
{
    const auto it = std::lower_bound(impl.records.begin(), impl.records.end(), id,
                                     [](const TopoName& r, const Hash256& h) { return r.id < h; });
    return it != impl.records.end() && it->id == id ? &*it : nullptr;
}

std::vector<std::byte> serializeTable(const ElementMap::Impl* impl)
{
    Writer w;
    w.raw(kMagic);
    const bool placements = impl != nullptr && !impl->placements.empty();
    w.u32(placements ? kVersion : kVersionNoPlacements);
    const std::size_t elementCount = impl != nullptr ? impl->elements.size() : 0;
    w.u32(static_cast<std::uint32_t>(elementCount));
    for (std::size_t i = 0; i < elementCount; ++i) {
        const ElementMap::Impl::Element& e = impl->elements[i];
        w.u8(static_cast<std::uint8_t>(e.sub.kind));
        w.u32(e.sub.index);
        const Fingerprint& f = e.fingerprint;
        w.u8(static_cast<std::uint8_t>(f.surface));
        for (const std::int64_t v : {f.qArea, f.qCx, f.qCy, f.qCz, f.qRadius, f.qAxisX, f.qAxisY, f.qAxisZ})
            w.i64(v);
        w.u16(f.neighbourCount);
        w.hashes(e.names);
    }
    const std::size_t recordCount = impl != nullptr ? impl->records.size() : 0;
    w.u32(static_cast<std::uint32_t>(recordCount));
    for (std::size_t i = 0; i < recordCount; ++i) {
        const TopoName& r = impl->records[i];
        w.hash(r.id);
        w.u8(static_cast<std::uint8_t>(r.kind));
        w.raw(r.origin.value.bytes);
        w.u32(static_cast<std::uint32_t>(r.role.size()));
        w.raw(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(r.role.data()), r.role.size()));
        w.hashes(r.parents);
        w.hashes(r.discriminator);
        w.u32(r.ordinal);
        w.u32(r.siblingCount);
        w.hash(r.family);
    }
    if (placements) {
        w.u32(static_cast<std::uint32_t>(impl->placements.size()));
        for (const FeaturePlacement& p : impl->placements) {
            w.raw(p.feature.value.bytes);
            for (const std::int64_t v : p.q)
                w.i64(v);
        }
    }
    const Hash256 sum = checksum(w.bytes());
    w.hash(sum);
    return std::move(w.bytes());
}

Result<ElementMap> deserializeTable(std::span<const std::byte> bytes)
{
    constexpr std::string_view where = "naming::ElementMap::deserialize";
    auto corrupt = [&](std::string why) {
        return tl::unexpected(makeError(ErrorCode::FormatError, "The element map is damaged: " + why + ".", where));
    };
    if (bytes.size() < kMagic.size() + 4 + 4 + 4 + 32)
        return corrupt("it is too short");
    const std::span<const std::byte> body = bytes.first(bytes.size() - 32);
    Hash256 stored;
    std::memcpy(stored.bytes.data(), bytes.data() + body.size(), 32);
    if (checksum(body) != stored)
        return corrupt("the checksum does not match");

    Reader r(body);
    std::array<std::uint8_t, 4> magic{};
    r.raw(magic);
    if (!r.ok() || magic != kMagic)
        return corrupt("it is not an element map");
    const std::uint32_t version = r.u32();
    if (r.ok() && version > kVersion)
        return tl::unexpected(
            makeError(ErrorCode::FormatTooNew, "The element map was written by a newer version of RayCAD.", where));
    if (!r.ok() || (version != kVersion && version != kVersionNoPlacements))
        return corrupt("unknown version");

    ElementMap::Impl table;
    table.elements.resize(r.count(kMinElementBytes));
    for (ElementMap::Impl::Element& e : table.elements) {
        e.sub.kind = static_cast<kernel::SubKind>(r.u8());
        e.sub.index = r.u32();
        const std::uint8_t surface = r.u8();
        if (!validSurface(surface))
            r.fail();
        Fingerprint& f = e.fingerprint;
        f.surface = static_cast<kernel::SurfaceType>(surface);
        f.qArea = r.i64();
        f.qCx = r.i64();
        f.qCy = r.i64();
        f.qCz = r.i64();
        f.qRadius = r.i64();
        f.qAxisX = r.i64();
        f.qAxisY = r.i64();
        f.qAxisZ = r.i64();
        f.neighbourCount = r.u16();
        e.names = r.hashes();
        if (!r.ok())
            return corrupt("an element entry is cut short or invalid");
    }
    table.records.resize(r.count(kMinRecordBytes));
    for (TopoName& n : table.records) {
        n.id = r.hash();
        n.kind = static_cast<kernel::SubKind>(r.u8());
        r.raw(n.origin.value.bytes);
        n.role = r.text(kMaxRoleLength);
        n.parents = r.hashes();
        n.discriminator = r.hashes();
        n.ordinal = r.u32();
        n.siblingCount = r.u32();
        n.family = r.hash();
        if (!r.ok())
            return corrupt("a name record is cut short or invalid");
    }
    if (version == kVersion) {
        table.placements.resize(r.count(kPlacementBytes));
        if (table.placements.empty())
            return corrupt("a version-2 map without placements");
        for (FeaturePlacement& p : table.placements) {
            r.raw(p.feature.value.bytes);
            for (std::int64_t& v : p.q)
                v = r.i64();
            if (!r.ok())
                return corrupt("a placement is cut short");
        }
    }
    if (!r.atEnd())
        return corrupt("unexpected bytes after the last record");
    return detail::finishMap(std::move(table), true);
}

} // namespace

// ---------- TopoName ----------

bool TopoName::ambiguous() const
{
    return ordinal != 0;
}

namespace detail {

Hash256 nameId(const TopoName& name)
{
    Hasher hasher;
    hasher.str("ray.naming.name.v1");
    hasher.i64(static_cast<std::int64_t>(name.kind));
    hashUuid(hasher, name.origin.value);
    hasher.str(name.role);
    hashList(hasher, name.parents);
    hashList(hasher, name.discriminator);
    hasher.i64(name.ordinal);
    hasher.i64(name.siblingCount);
    return hasher.finish();
}

Hash256 familyId(kernel::SubKind kind, const FeatureId& origin, std::string_view role, std::span<const Hash256> parents)
{
    Hasher hasher;
    hasher.str("ray.naming.family.v1");
    hasher.i64(static_cast<std::int64_t>(kind));
    hashUuid(hasher, origin.value);
    hasher.str(role);
    hashList(hasher, parents);
    return hasher.finish();
}

Result<ElementMap> finishMap(ElementMap::Impl table, bool fromFile)
{
    constexpr std::string_view where = "naming::ElementMap";
    auto bad = [&](const std::string& why) {
        const ErrorCode code = fromFile ? ErrorCode::FormatError : ErrorCode::Internal;
        const std::string prefix = fromFile ? "The element map is damaged: " : "An element map broke a rule: ";
        return tl::unexpected(makeError(code, prefix + why + ".", where));
    };

    for (std::size_t i = 0; i < table.elements.size(); ++i) {
        const ElementMap::Impl::Element& e = table.elements[i];
        if (!validKind(e.sub.kind))
            return bad("unknown element kind");
        if (i > 0 && !(table.elements[i - 1].sub < e.sub))
            return bad("elements are not in order");
        if (e.names.empty())
            return bad("an element has no name");
    }
    for (std::size_t i = 0; i < table.records.size(); ++i) {
        const TopoName& n = table.records[i];
        if (!validKind(n.kind))
            return bad("unknown name kind");
        if (i > 0 && !(table.records[i - 1].id < n.id))
            return bad("names are not in order");
        if (!std::is_sorted(n.parents.begin(), n.parents.end()) ||
            !std::is_sorted(n.discriminator.begin(), n.discriminator.end()))
            return bad("a name's parents or discriminator are not sorted");
        if (n.siblingCount == 0 || n.ordinal > n.siblingCount)
            return bad("a name has an impossible ordinal");
        if (nameId(n) != n.id)
            return bad("a name does not match its fields");
        if (familyId(n.kind, n.origin, n.role, n.parents) != n.family)
            return bad("a name's family does not match its fields");
    }

    std::vector<std::pair<Hash256, kernel::SubId>> holders;
    for (const ElementMap::Impl::Element& e : table.elements)
        for (const Hash256& name : e.names)
            holders.emplace_back(name, e.sub);
    std::sort(holders.begin(), holders.end());
    std::vector<bool> used(table.records.size(), false);
    for (std::size_t i = 0; i < holders.size();) {
        std::size_t end = i + 1;
        while (end < holders.size() && holders[end].first == holders[i].first)
            ++end;
        const TopoName* record = findRecord(table, holders[i].first);
        if (record == nullptr)
            return bad("an element has a name without a record");
        used[static_cast<std::size_t>(record - table.records.data())] = true;
        for (std::size_t k = i; k < end; ++k) {
            if (holders[k].second.kind != record->kind)
                return bad("a name is held by an element of another kind");
            if (k > i && holders[k].second == holders[k - 1].second)
                return bad("an element holds the same name twice");
        }
        if (end - i == 1)
            table.byName.push_back(holders[i]);
        i = end;
    }
    if (std::find(used.begin(), used.end(), false) != used.end())
        return bad("a name record is not held by any element");
    for (std::size_t i = 1; i < table.placements.size(); ++i)
        if (!(table.placements[i - 1].feature < table.placements[i].feature))
            return bad("placements are not in order");
    for (std::size_t i = 1; i < table.ancestors.size(); ++i)
        if (!(table.ancestors[i - 1].id < table.ancestors[i].id))
            return bad("ancestor records are not in order");
    for (std::size_t i = 1; i < table.runs.size(); ++i)
        if (!(table.runs[i - 1].first < table.runs[i].first))
            return bad("one-to-one runs are not in order");
    for (std::size_t i = 1; i < table.wholeBlends.size(); ++i)
        if (!(table.wholeBlends[i - 1] < table.wholeBlends[i]))
            return bad("whole blend names are not in order");

    std::vector<std::pair<Hash256, Hash256>> members;
    members.reserve(table.records.size());
    for (const TopoName& n : table.records)
        members.emplace_back(n.family, n.id);
    std::sort(members.begin(), members.end());
    for (const auto& [family, name] : members) {
        if (table.families.empty() || table.families.back().first != family)
            table.families.emplace_back(family, std::vector<Hash256>{});
        table.families.back().second.push_back(name);
    }

    ElementMap map;
    map.impl = std::make_shared<const ElementMap::Impl>(std::move(table));
    return map;
}

const TopoName* recordOrAncestor(const ElementMap& map, const Hash256& name)
{
    if (const TopoName* record = map.record(name))
        return record;
    if (!map.impl)
        return nullptr;
    const std::vector<TopoName>& a = map.impl->ancestors;
    const auto it =
        std::lower_bound(a.begin(), a.end(), name, [](const TopoName& r, const Hash256& h) { return r.id < h; });
    return it != a.end() && it->id == name ? &*it : nullptr;
}

std::vector<const FeaturePlacement*> bodyMovesOf(const ElementMap& map, const FeatureId& feature)
{
    std::vector<const FeaturePlacement*> out;
    if (!map.impl)
        return out;
    for (const ElementMap::Impl::BodyMove& m : map.impl->bodyMoves)
        if (std::binary_search(m.carried.begin(), m.carried.end(), feature))
            out.push_back(&m.placement);
    return out;
}

std::vector<const FeaturePlacement*> faceMovesOf(const ElementMap& map, std::span<const Hash256> names)
{
    std::vector<const FeaturePlacement*> out;
    if (!map.impl)
        return out;
    for (const ElementMap::Impl::FaceMoveRecord& m : map.impl->faceMoves)
        if (std::any_of(names.begin(), names.end(),
                        [&](const Hash256& n) { return std::binary_search(m.names.begin(), m.names.end(), n); }))
            out.push_back(&m.placement);
    return out;
}

std::optional<FeatureId> runOf(const ElementMap& map, kernel::SubId face)
{
    if (!map.impl)
        return std::nullopt;
    const auto& runs = map.impl->runs;
    const auto it =
        std::lower_bound(runs.begin(), runs.end(), face,
                         [](const std::pair<kernel::SubId, FeatureId>& r, kernel::SubId s) { return r.first < s; });
    if (it == runs.end() || it->first != face)
        return std::nullopt;
    return it->second;
}

bool wholeBlend(const ElementMap& map, const Hash256& name)
{
    return map.impl && std::binary_search(map.impl->wholeBlends.begin(), map.impl->wholeBlends.end(), name);
}

} // namespace detail

// ---------- ElementMap ----------

std::optional<kernel::SubId> ElementMap::find(const Hash256& nameId) const
{
    if (!impl)
        return std::nullopt;
    const auto it = std::lower_bound(
        impl->byName.begin(), impl->byName.end(), nameId,
        [](const std::pair<Hash256, kernel::SubId>& entry, const Hash256& h) { return entry.first < h; });
    if (it == impl->byName.end() || it->first != nameId)
        return std::nullopt;
    return it->second;
}

std::span<const Hash256> ElementMap::namesOf(kernel::SubId sub) const
{
    if (!impl)
        return {};
    const Impl::Element* e = findElement(*impl, sub);
    return e != nullptr ? std::span<const Hash256>(e->names) : std::span<const Hash256>();
}

const TopoName* ElementMap::record(const Hash256& nameId) const
{
    return impl ? findRecord(*impl, nameId) : nullptr;
}

std::span<const Hash256> ElementMap::family(const Hash256& familyId) const
{
    if (!impl)
        return {};
    const auto it = std::lower_bound(
        impl->families.begin(), impl->families.end(), familyId,
        [](const std::pair<Hash256, std::vector<Hash256>>& entry, const Hash256& h) { return entry.first < h; });
    if (it == impl->families.end() || it->first != familyId)
        return {};
    return it->second;
}

Fingerprint ElementMap::fingerprintOf(kernel::SubId sub) const
{
    const Impl::Element* e = impl ? findElement(*impl, sub) : nullptr;
    return e != nullptr ? e->fingerprint : detail::emptyFingerprint();
}

std::size_t ElementMap::size() const
{
    return impl ? impl->elements.size() : 0;
}

const FeaturePlacement* ElementMap::placement(FeatureId feature) const
{
    if (!impl)
        return nullptr;
    const auto it = std::lower_bound(impl->placements.begin(), impl->placements.end(), feature,
                                     [](const FeaturePlacement& p, const FeatureId& f) { return p.feature < f; });
    return it != impl->placements.end() && it->feature == feature ? &*it : nullptr;
}

FeaturePlacement placementOf(FeatureId feature, const Xform& frameToModel)
{
    FeaturePlacement p{feature, {}};
    for (std::size_t i = 0; i < 12; ++i)
        p.q[i] = (i % 4 == 3) ? detail::quantise(frameToModel.m[i], detail::kLengthQuantaPerMm)
                              : detail::quantise(frameToModel.m[i], 1e9);
    return p;
}

Xform sweepPlacement(const kernel::Plane& plane, std::span<const Vec2> points)
{
    double lo[2] = {0, 0}, hi[2] = {0, 0};
    for (std::size_t i = 0; i < points.size(); ++i) {
        const double v[2] = {points[i].x, points[i].y};
        for (int k = 0; k < 2; ++k) {
            lo[k] = i == 0 ? v[k] : std::min(lo[k], v[k]);
            hi[k] = i == 0 ? v[k] : std::max(hi[k], v[k]);
        }
    }
    const double u = 0.5 * (lo[0] + hi[0]), w = 0.5 * (lo[1] + hi[1]);
    const Vec3 x = plane.xDir;
    const Vec3 n = plane.normal;
    const Vec3 y{n.y * x.z - n.z * x.y, n.z * x.x - n.x * x.z, n.x * x.y - n.y * x.x};
    const Vec3 o{plane.origin.x + x.x * u + y.x * w, plane.origin.y + x.y * u + y.y * w,
                 plane.origin.z + x.z * u + y.z * w};
    return Xform{{x.x, y.x, n.x, o.x, x.y, y.y, n.y, o.y, x.z, y.z, n.z, o.z}};
}

Xform sweepPlacement(const kernel::Plane& plane, std::span<const kernel::ProfileLoop> loops)
{
    std::vector<Vec2> points;
    for (const kernel::ProfileLoop& loop : loops)
        for (const kernel::ProfileCurve& c : loop.curves) {
            if (c.kind == kernel::ProfileCurve::Kind::Circle && !c.pts.empty()) {
                points.push_back(Vec2{c.pts[0].x - c.radius, c.pts[0].y - c.radius});
                points.push_back(Vec2{c.pts[0].x + c.radius, c.pts[0].y + c.radius});
                continue;
            }
            points.insert(points.end(), c.pts.begin(), c.pts.end());
        }
    return sweepPlacement(plane, std::span<const Vec2>(points));
}

std::optional<FaceMove> offsetFaceMove(const kernel::Shape& shape, kernel::SubId face, double distance)
{
    try {
        if (face.kind != kernel::SubKind::Face || !std::isfinite(distance))
            return std::nullopt;
        const auto info = kernel::surfaceInfo(shape, face);
        if (!info || info->type != kernel::SurfaceType::Plane)
            return std::nullopt;
        // The outward normal from the kernel's face sample (the plane's own axis may point either way).
        const auto sample = kernel::faceSample(shape, face);
        if (!sample)
            return std::nullopt;
        const Vec3 n = sample->normal;
        const double len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        if (!(len > 1e-12))
            return std::nullopt;
        const double k = distance / len;
        return FaceMove{face, Xform{{1, 0, 0, n.x * k, 0, 1, 0, n.y * k, 0, 0, 1, n.z * k}}};
    } catch (...) {
        return std::nullopt;
    }
}

bool samePlacement(const FeaturePlacement& a, const FeaturePlacement& b)
{
    for (std::size_t i = 0; i < 12; ++i) {
        const std::int64_t d = a.q[i] > b.q[i] ? a.q[i] - b.q[i] : b.q[i] - a.q[i];
        if (d > ((i % 4 == 3) ? 10 : 1000))
            return false;
    }
    return a.feature == b.feature;
}

std::vector<std::byte> ElementMap::serialize() const
{
    try {
        return serializeTable(impl.get());
    } catch (...) {
        // Only std::bad_alloc can get here. An empty result is never a valid map, so deserialize rejects it.
        return {};
    }
}

Result<ElementMap> ElementMap::deserialize(std::span<const std::byte> bytes)
{
    try {
        return deserializeTable(bytes);
    } catch (const std::bad_alloc&) {
        return tl::unexpected(
            makeError(ErrorCode::Internal, "Out of memory while reading an element map.", "naming::ElementMap"));
    } catch (...) {
        return tl::unexpected(
            makeError(ErrorCode::Internal, "Unexpected failure while reading an element map.", "naming::ElementMap"));
    }
}

} // namespace ray::naming
