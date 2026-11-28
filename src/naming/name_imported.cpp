// nameImported: names for a history-free body (task T-007). The rule is described in import_naming.h.
// Deterministic: every grouping is done by sorting on hashes and quantised geometry, never on the order in
// which the kernel lists sub-shapes.
#include "import_naming.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <new>
#include <numeric>
#include <string>

namespace ray::naming {

namespace detail {

namespace {

constexpr std::string_view kFaceRole = "import.face";
constexpr std::string_view kEdgeRole = "import.edge";
constexpr std::string_view kVertexRole = "import.vertex";

Error makeError(ErrorCode code, std::string message, std::string_view where)
{
    Error error{code, std::move(message), {}};
    error.context.emplace_back(where);
    return error;
}

std::uint16_t clampCount(std::size_t n) noexcept
{
    return static_cast<std::uint16_t>(std::min<std::size_t>(n, std::numeric_limits<std::uint16_t>::max()));
}

void sortUnique(std::vector<Hash256>& v)
{
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

// v minus every entry of `sortedExclude` (both sorted); keeps v sorted.
std::vector<Hash256> without(const std::vector<Hash256>& v, const std::vector<Hash256>& sortedExclude)
{
    std::vector<Hash256> out;
    std::set_difference(v.begin(), v.end(), sortedExclude.begin(), sortedExclude.end(), std::back_inserter(out));
    return out;
}

// One element of one kind, ready to be named.
struct Item {
    std::uint32_t index{};
    std::vector<Hash256> parents; // sorted
    Fingerprint fingerprint{};
    std::array<std::int64_t, 6> ends{};
};

// Discriminator of items[pos]. `provisional` holds, per SubId index of this kind, the item's name if it is alone
// in its family, else its family id.
using DiscriminatorFn = std::function<std::vector<Hash256>(std::size_t pos, std::span<const Hash256> provisional)>;

struct KindNames {
    std::vector<Hash256> byIndex; // per SubId index
    std::vector<TopoName> records;
};

TopoName makeName(kernel::SubKind kind, const FeatureId& origin, std::string_view role, const Item& item,
                  const Hash256& family, std::vector<Hash256> discriminator, std::uint32_t ordinal,
                  std::uint32_t siblingCount)
{
    TopoName name{};
    name.kind = kind;
    name.origin = origin;
    name.role = std::string(role);
    name.parents = item.parents;
    name.discriminator = std::move(discriminator);
    name.ordinal = ordinal;
    name.siblingCount = siblingCount;
    name.family = family;
    name.id = nameId(name);
    return name;
}

// Order of two items in a tie group: fingerprint (ordinalOrder), then end points.
std::strong_ordering tieOrder(const Item& a, const Item& b) noexcept
{
    if (const auto c = ordinalOrder(a.fingerprint, b.fingerprint); c != 0)
        return c;
    return a.ends <=> b.ends;
}

KindNames nameKind(kernel::SubKind kind, const FeatureId& origin, std::string_view role, std::uint32_t count,
                   const std::vector<Item>& items, const DiscriminatorFn& discriminatorOf)
{
    const std::size_t n = items.size();
    std::vector<Hash256> families(n);
    for (std::size_t i = 0; i < n; ++i)
        families[i] = familyId(kind, origin, role, items[i].parents);

    // Family sizes: sort positions by family id (ties by SubId index, which is only used to make the sort
    // total; nothing below depends on the order inside a run).
    std::vector<std::size_t> byFamily(n);
    std::iota(byFamily.begin(), byFamily.end(), std::size_t{0});
    std::sort(byFamily.begin(), byFamily.end(), [&](std::size_t a, std::size_t b) {
        return families[a] != families[b] ? families[a] < families[b] : items[a].index < items[b].index;
    });
    std::vector<std::uint32_t> familySize(n, 0);
    for (std::size_t i = 0; i < n;) {
        std::size_t end = i + 1;
        while (end < n && families[byFamily[end]] == families[byFamily[i]])
            ++end;
        for (std::size_t k = i; k < end; ++k)
            familySize[byFamily[k]] = static_cast<std::uint32_t>(end - i);
        i = end;
    }

    KindNames out;
    out.byIndex.assign(count, Hash256{});
    std::vector<Hash256> provisional(count, Hash256{});
    std::vector<std::size_t> withSiblings;
    for (std::size_t i = 0; i < n; ++i) {
        if (familySize[i] == 1) {
            TopoName name = makeName(kind, origin, role, items[i], families[i], {}, 0, 1);
            out.byIndex[items[i].index] = name.id;
            provisional[items[i].index] = name.id;
            out.records.push_back(std::move(name));
        } else {
            provisional[items[i].index] = families[i];
            withSiblings.push_back(i);
        }
    }

    // Siblings: discriminator first, then an ordinal among those whose discriminators are identical.
    std::vector<std::vector<Hash256>> discriminators(n);
    for (const std::size_t i : withSiblings)
        discriminators[i] = discriminatorOf(i, provisional);
    std::sort(withSiblings.begin(), withSiblings.end(), [&](std::size_t a, std::size_t b) {
        if (families[a] != families[b])
            return families[a] < families[b];
        if (discriminators[a] != discriminators[b])
            return discriminators[a] < discriminators[b];
        if (const auto c = tieOrder(items[a], items[b]); c != 0)
            return c < 0;
        return items[a].index < items[b].index; // only to make the sort total; equal items get equal names
    });
    for (std::size_t i = 0; i < withSiblings.size();) {
        const std::size_t first = withSiblings[i];
        std::size_t end = i + 1;
        while (end < withSiblings.size() && families[withSiblings[end]] == families[first] &&
               discriminators[withSiblings[end]] == discriminators[first])
            ++end;
        const bool needsOrdinal = end - i > 1;
        std::uint32_t ordinal = needsOrdinal ? 1 : 0;
        for (std::size_t k = i; k < end; ++k) {
            const std::size_t pos = withSiblings[k];
            if (k > i && tieOrder(items[withSiblings[k - 1]], items[pos]) != 0)
                ++ordinal;
            TopoName name =
                makeName(kind, origin, role, items[pos], families[pos], discriminators[pos], ordinal, familySize[pos]);
            out.byIndex[items[pos].index] = name.id;
            out.records.push_back(std::move(name));
        }
        i = end;
    }
    return out;
}

// Every index in 0..count-1 exactly once.
bool isPermutation(const std::vector<std::uint32_t>& indices, std::uint32_t count)
{
    if (indices.size() != count)
        return false;
    std::vector<bool> seen(count, false);
    for (const std::uint32_t i : indices) {
        if (i >= count || seen[i])
            return false;
        seen[i] = true;
    }
    return true;
}

bool allBelow(const std::vector<std::uint32_t>& refs, std::uint32_t count)
{
    return std::all_of(refs.begin(), refs.end(), [&](std::uint32_t r) { return r < count; });
}

std::vector<std::uint32_t> indicesOf(const std::vector<kernel::SubId>& subs)
{
    std::vector<std::uint32_t> out;
    out.reserve(subs.size());
    for (const kernel::SubId& s : subs)
        out.push_back(s.index);
    return out;
}

std::vector<Hash256> namesAt(const std::vector<std::uint32_t>& faces, const std::vector<Hash256>& faceNames)
{
    std::vector<Hash256> out;
    out.reserve(faces.size());
    for (const std::uint32_t f : faces)
        out.push_back(faceNames[f]);
    sortUnique(out);
    return out;
}

} // namespace

Result<BodyFacts> gatherFacts(const kernel::Shape& shape, const VisitOrder& order)
{
    constexpr std::string_view where = "naming::nameImported";
    if (shape.isNull())
        return tl::unexpected(makeError(ErrorCode::KernelFailure, "The shape is empty.", where));

    BodyFacts facts;
    facts.faceCount = shape.count(kernel::SubKind::Face);
    facts.edgeCount = shape.count(kernel::SubKind::Edge);
    facts.vertexCount = shape.count(kernel::SubKind::Vertex);

    auto visiting = [&](kernel::SubKind kind, std::uint32_t count) -> Result<std::vector<std::uint32_t>> {
        std::vector<std::uint32_t> indices;
        if (order) {
            indices = order(kind, count);
            if (!isPermutation(indices, count))
                return tl::unexpected(
                    makeError(ErrorCode::InvalidArgument, "The visiting order is not a permutation.", where));
        } else {
            indices.resize(count);
            std::iota(indices.begin(), indices.end(), std::uint32_t{0});
        }
        return indices;
    };
    auto failed = [&](const Error& error) {
        Error out = error;
        out.context.emplace_back(where);
        return tl::unexpected(std::move(out));
    };

    auto faceOrder = visiting(kernel::SubKind::Face, facts.faceCount);
    if (!faceOrder)
        return tl::unexpected(faceOrder.error());
    for (const std::uint32_t i : *faceOrder) {
        const kernel::SubId face{kernel::SubKind::Face, i};
        auto info = kernel::surfaceInfo(shape, face);
        if (!info)
            return failed(info.error());
        FaceFacts f;
        f.index = i;
        f.neighbours = indicesOf(kernel::adjacent(shape, face, kernel::SubKind::Face));
        f.fingerprint = faceFingerprint(*info, clampCount(f.neighbours.size()));
        facts.faces.push_back(std::move(f));
    }

    auto edgeOrder = visiting(kernel::SubKind::Edge, facts.edgeCount);
    if (!edgeOrder)
        return tl::unexpected(edgeOrder.error());
    for (const std::uint32_t i : *edgeOrder) {
        const kernel::SubId edge{kernel::SubKind::Edge, i};
        auto info = kernel::curveInfo(shape, edge);
        if (!info)
            return failed(info.error());
        EdgeFacts e;
        e.index = i;
        e.faces = indicesOf(kernel::adjacent(shape, edge, kernel::SubKind::Face));
        e.vertices = indicesOf(kernel::adjacent(shape, edge, kernel::SubKind::Vertex));
        e.fingerprint = edgeFingerprint(*info, clampCount(e.faces.size()));
        e.ends = {quantise(info->start.x, kLengthQuantaPerMm), quantise(info->start.y, kLengthQuantaPerMm),
                  quantise(info->start.z, kLengthQuantaPerMm), quantise(info->end.x, kLengthQuantaPerMm),
                  quantise(info->end.y, kLengthQuantaPerMm),   quantise(info->end.z, kLengthQuantaPerMm)};
        facts.edges.push_back(std::move(e));
    }

    auto vertexOrder = visiting(kernel::SubKind::Vertex, facts.vertexCount);
    if (!vertexOrder)
        return tl::unexpected(vertexOrder.error());
    for (const std::uint32_t i : *vertexOrder) {
        const kernel::SubId vertex{kernel::SubKind::Vertex, i};
        auto point = kernel::vertexPoint(shape, vertex);
        if (!point)
            return failed(point.error());
        VertexFacts v;
        v.index = i;
        v.faces = indicesOf(kernel::adjacent(shape, vertex, kernel::SubKind::Face));
        v.edges = indicesOf(kernel::adjacent(shape, vertex, kernel::SubKind::Edge));
        v.fingerprint = vertexFingerprint(*point, clampCount(v.faces.size()));
        facts.vertices.push_back(std::move(v));
    }
    return facts;
}

Result<ElementMap> assignNames(const FeatureId& importFeature, const BodyFacts& facts)
{
    constexpr std::string_view where = "naming::nameImported";
    auto inconsistent = [&](const char* why) {
        return tl::unexpected(
            makeError(ErrorCode::InvalidArgument, std::string("Inconsistent body facts: ") + why, where));
    };

    // ---- consistency: every index once, every reference in range ----
    {
        std::vector<std::uint32_t> faceIdx, edgeIdx, vertexIdx;
        for (const FaceFacts& f : facts.faces) {
            faceIdx.push_back(f.index);
            if (!allBelow(f.neighbours, facts.faceCount))
                return inconsistent("face neighbour out of range");
        }
        for (const EdgeFacts& e : facts.edges) {
            edgeIdx.push_back(e.index);
            if (!allBelow(e.faces, facts.faceCount) || !allBelow(e.vertices, facts.vertexCount))
                return inconsistent("edge reference out of range");
        }
        for (const VertexFacts& v : facts.vertices) {
            vertexIdx.push_back(v.index);
            if (!allBelow(v.faces, facts.faceCount) || !allBelow(v.edges, facts.edgeCount))
                return inconsistent("vertex reference out of range");
        }
        if (!isPermutation(faceIdx, facts.faceCount) || !isPermutation(edgeIdx, facts.edgeCount) ||
            !isPermutation(vertexIdx, facts.vertexCount))
            return inconsistent("an element is missing or listed twice");
    }

    // Lookup tables by SubId index (lookups only; nothing is emitted in this order except the final map).
    std::vector<const FaceFacts*> faceAt(facts.faceCount);
    for (const FaceFacts& f : facts.faces)
        faceAt[f.index] = &f;
    std::vector<const EdgeFacts*> edgeAt(facts.edgeCount);
    for (const EdgeFacts& e : facts.edges)
        edgeAt[e.index] = &e;
    std::vector<const VertexFacts*> vertexAt(facts.vertexCount);
    for (const VertexFacts& v : facts.vertices)
        vertexAt[v.index] = &v;

    // ---- faces ----
    std::vector<Item> faceItems;
    faceItems.reserve(facts.faces.size());
    for (const FaceFacts& f : facts.faces)
        faceItems.push_back(Item{f.index, {fingerprintKey(f.fingerprint)}, f.fingerprint, {}});
    const KindNames faces = nameKind(kernel::SubKind::Face, importFeature, kFaceRole, facts.faceCount, faceItems,
                                     [&](std::size_t pos, std::span<const Hash256> provisional) {
                                         std::vector<Hash256> d;
                                         for (const std::uint32_t nb : facts.faces[pos].neighbours)
                                             if (nb != facts.faces[pos].index)
                                                 d.push_back(provisional[nb]);
                                         std::sort(d.begin(), d.end());
                                         return d;
                                     });

    // ---- edges ----
    std::vector<Item> edgeItems;
    edgeItems.reserve(facts.edges.size());
    for (const EdgeFacts& e : facts.edges) {
        Item item{e.index, namesAt(e.faces, faces.byIndex), e.fingerprint, e.ends};
        if (item.parents.empty())
            item.parents.push_back(fingerprintKey(e.fingerprint));
        edgeItems.push_back(std::move(item));
    }
    const KindNames edges = nameKind(kernel::SubKind::Edge, importFeature, kEdgeRole, facts.edgeCount, edgeItems,
                                     [&](std::size_t pos, std::span<const Hash256>) {
                                         std::vector<Hash256> around;
                                         for (const std::uint32_t v : facts.edges[pos].vertices)
                                             for (const std::uint32_t f : vertexAt[v]->faces)
                                                 around.push_back(faces.byIndex[f]);
                                         sortUnique(around);
                                         return without(around, edgeItems[pos].parents);
                                     });

    // ---- vertices ----
    std::vector<Item> vertexItems;
    vertexItems.reserve(facts.vertices.size());
    for (const VertexFacts& v : facts.vertices) {
        Item item{v.index, namesAt(v.faces, faces.byIndex), v.fingerprint, {}};
        if (item.parents.empty())
            item.parents.push_back(fingerprintKey(v.fingerprint));
        vertexItems.push_back(std::move(item));
    }
    const KindNames vertices = nameKind(kernel::SubKind::Vertex, importFeature, kVertexRole, facts.vertexCount,
                                        vertexItems, [&](std::size_t pos, std::span<const Hash256>) {
                                            const VertexFacts& self = facts.vertices[pos];
                                            std::vector<Hash256> around;
                                            for (const std::uint32_t e : self.edges)
                                                for (const std::uint32_t far : edgeAt[e]->vertices)
                                                    if (far != self.index)
                                                        for (const std::uint32_t f : vertexAt[far]->faces)
                                                            around.push_back(faces.byIndex[f]);
                                            sortUnique(around);
                                            return without(around, vertexItems[pos].parents);
                                        });

    // ---- the table, in SubId order ----
    ElementMap::Impl table;
    table.elements.reserve(facts.faceCount + facts.edgeCount + facts.vertexCount);
    for (std::uint32_t i = 0; i < facts.vertexCount; ++i)
        table.elements.push_back(
            {kernel::SubId{kernel::SubKind::Vertex, i}, vertexAt[i]->fingerprint, {vertices.byIndex[i]}});
    for (std::uint32_t i = 0; i < facts.edgeCount; ++i)
        table.elements.push_back({kernel::SubId{kernel::SubKind::Edge, i}, edgeAt[i]->fingerprint, {edges.byIndex[i]}});
    for (std::uint32_t i = 0; i < facts.faceCount; ++i)
        table.elements.push_back({kernel::SubId{kernel::SubKind::Face, i}, faceAt[i]->fingerprint, {faces.byIndex[i]}});
    for (const KindNames* kind : {&faces, &edges, &vertices})
        table.records.insert(table.records.end(), kind->records.begin(), kind->records.end());
    std::sort(table.records.begin(), table.records.end(),
              [](const TopoName& a, const TopoName& b) { return a.id < b.id; });
    table.records.erase(std::unique(table.records.begin(), table.records.end(),
                                    [](const TopoName& a, const TopoName& b) { return a.id == b.id; }),
                        table.records.end());
    // (T-081b round 11: an imported face's name is issued here; its run of one-to-one history starts with the import,
    // ElementMap::Impl::runs.)
    for (std::uint32_t i = 0; i < facts.faceCount; ++i)
        table.runs.emplace_back(kernel::SubId{kernel::SubKind::Face, i}, importFeature);
    return finishMap(std::move(table), false);
}

ElementMap nameImported(const FeatureId& importFeature, const kernel::Shape& shape, const VisitOrder& order)
{
    std::string why;
    try {
        auto facts = gatherFacts(shape, order);
        if (facts) {
            auto map = assignNames(importFeature, *facts);
            if (map)
                return std::move(*map);
            why = map.error().message;
        } else {
            why = facts.error().message;
        }
    } catch (const std::bad_alloc&) {
        why = "out of memory";
    } catch (...) {
        why = "unexpected failure";
    }
    try {
        ray::log(LogLevel::Warn, "naming", "nameImported produced no names: " + why);
    } catch (...) {
        // logging is best effort
    }
    return ElementMap{};
}

} // namespace detail

ElementMap nameImported(FeatureId importFeature, const kernel::Shape& shape)
{
    return detail::nameImported(importFeature, shape, detail::VisitOrder{});
}

} // namespace ray::naming
