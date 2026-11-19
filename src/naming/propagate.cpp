// propagate(): names for the result of a modelling operation (task T-081a). The rules are in propagate.h.
#include "propagate.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace ray::naming {

namespace detail {

Hash256 curveTagParent(std::uint64_t curveTag)
{
    Hasher hasher;
    hasher.str("ray.naming.sketch-curve.v1");
    hasher.i64(static_cast<std::int64_t>(curveTag));
    return hasher.finish();
}

Hash256 loopTagParent(std::uint64_t loopTag)
{
    Hasher hasher;
    hasher.str("ray.naming.sketch-loop.v1");
    hasher.i64(static_cast<std::int64_t>(loopTag));
    return hasher.finish();
}

Hash256 allFacesDiscriminatorMark()
{
    Hasher hasher;
    hasher.str("ray.naming.discriminator.all-faces.v1");
    return hasher.finish();
}

bool needsHint(const TopoName& name)
{
    if (name.ambiguous())
        return true;
    // An edge or vertex among siblings: what tells it from them are the faces at its ends, and the pieces of an edge
    // trade territory when the feature between them moves while every piece keeps its end faces (random 9:41, 2:40).
    // (Names of an imported body are issued once from the imported shape, which edits do not change.)
    if (name.siblingCount > 1 && name.kind != kernel::SubKind::Face && name.role.rfind("import.", 0) != 0)
        return true;
    static const Hash256 mark = allFacesDiscriminatorMark();
    return name.siblingCount > 1 && std::binary_search(name.discriminator.begin(), name.discriminator.end(), mark);
}

} // namespace detail

namespace {

using kernel::SubId;
using kernel::SubKind;

constexpr std::string_view kWhere = "naming::propagate";
// The three named kinds, in naming order (faces first: edges and vertices are named from faces).
constexpr std::array<SubKind, 3> kNamedKinds{SubKind::Face, SubKind::Edge, SubKind::Vertex};

Error makeError(ErrorCode code, std::string message)
{
    Error error{code, std::move(message), {}};
    error.context.emplace_back(kWhere);
    return error;
}

std::size_t slot(SubKind kind)
{
    return static_cast<std::size_t>(kind);
}

char kindLetter(SubKind kind)
{
    switch (kind) {
    case SubKind::Vertex:
        return 'V';
    case SubKind::Edge:
        return 'E';
    case SubKind::Face:
        return 'F';
    case SubKind::Solid:
        return 'S';
    }
    return '?';
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

void sortUnique(std::vector<std::uint32_t>& v)
{
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

std::vector<std::uint32_t> indicesOf(const std::vector<SubId>& subs)
{
    std::vector<std::uint32_t> out;
    out.reserve(subs.size());
    for (const SubId& s : subs)
        out.push_back(s.index);
    return out;
}

// Topology and fingerprints of the result, gathered once.
struct Topology {
    std::array<std::uint32_t, 3> count{}; // by slot (Vertex, Edge, Face)
    std::vector<std::vector<std::uint32_t>> faceNeighbours, faceEdges;
    std::vector<std::vector<std::uint32_t>> edgeFaces, edgeVertices;
    std::vector<std::vector<std::uint32_t>> vertexFaces, vertexEdges;
    std::array<std::vector<Fingerprint>, 3> fingerprint; // by slot
};

Topology gather(const kernel::Shape& shape)
{
    Topology t;
    for (const SubKind kind : kNamedKinds)
        t.count[slot(kind)] = shape.count(kind);
    const std::uint32_t faces = t.count[slot(SubKind::Face)];
    const std::uint32_t edges = t.count[slot(SubKind::Edge)];
    const std::uint32_t vertices = t.count[slot(SubKind::Vertex)];
    t.faceNeighbours.resize(faces);
    t.faceEdges.resize(faces);
    t.edgeFaces.resize(edges);
    t.edgeVertices.resize(edges);
    t.vertexFaces.resize(vertices);
    t.vertexEdges.resize(vertices);
    for (const SubKind kind : kNamedKinds)
        t.fingerprint[slot(kind)].assign(t.count[slot(kind)], detail::emptyFingerprint());

    // A query that fails leaves the empty fingerprint: such an element can only get an ordinal tie (and so a
    // name shared with its twins, which resolves to nothing), never somebody else's name.
    for (std::uint32_t i = 0; i < faces; ++i) {
        const SubId face{SubKind::Face, i};
        t.faceNeighbours[i] = indicesOf(kernel::adjacent(shape, face, SubKind::Face));
        t.faceEdges[i] = indicesOf(kernel::adjacent(shape, face, SubKind::Edge));
        if (auto info = kernel::surfaceInfo(shape, face))
            t.fingerprint[slot(SubKind::Face)][i] =
                detail::faceFingerprint(*info, clampCount(t.faceNeighbours[i].size()));
    }
    for (std::uint32_t i = 0; i < edges; ++i) {
        const SubId edge{SubKind::Edge, i};
        t.edgeFaces[i] = indicesOf(kernel::adjacent(shape, edge, SubKind::Face));
        t.edgeVertices[i] = indicesOf(kernel::adjacent(shape, edge, SubKind::Vertex));
        if (auto info = kernel::curveInfo(shape, edge))
            t.fingerprint[slot(SubKind::Edge)][i] = detail::edgeFingerprint(*info, clampCount(t.edgeFaces[i].size()));
    }
    for (std::uint32_t i = 0; i < vertices; ++i) {
        const SubId vertex{SubKind::Vertex, i};
        t.vertexFaces[i] = indicesOf(kernel::adjacent(shape, vertex, SubKind::Face));
        t.vertexEdges[i] = indicesOf(kernel::adjacent(shape, vertex, SubKind::Edge));
        if (auto point = kernel::vertexPoint(shape, vertex))
            t.fingerprint[slot(SubKind::Vertex)][i] =
                detail::vertexFingerprint(*point, clampCount(t.vertexFaces[i].size()));
    }
    return t;
}

// A name waiting for its discriminator and ordinal.
struct Pending {
    SubId element;
    std::string role;
    std::vector<Hash256> parents; // sorted, unique
    int sourceInput = -1;         // for a split piece: the input element it is a piece of
    SubId source{};
    Hash256 family{};
};

using SourceKey = std::pair<int, SubId>;

class Propagator {
public:
    Propagator(const NamingInput& in, Topology topology) : in_(in), t_(std::move(topology))
    {
        for (const SubKind kind : kNamedKinds)
            names_[slot(kind)].resize(t_.count[slot(kind)]);
    }

    Result<NamingOutput> run()
    {
        nameSweepFaces();
        if (auto ok = indexHistory(); !ok)
            return tl::unexpected(ok.error());
        for (const SubKind kind : kNamedKinds)
            nameFromHistory(kind);
        nameFromNeighbours();
        NamingOutput out;
        out.lineage = lineage();
        auto map = finish();
        if (!map)
            return tl::unexpected(map.error());
        out.map = std::move(*map);
        return out;
    }

private:
    // ---------- state ----------
    bool named(SubId s) const { return !names_[slot(s.kind)][s.index].empty(); }

    // Every name of input element `s` (empty if the input map has none).
    std::span<const Hash256> inputNames(const SourceKey& s) const
    {
        const ElementMap* map = in_.inputMaps[static_cast<std::size_t>(s.first)];
        return map != nullptr ? map->namesOf(s.second) : std::span<const Hash256>();
    }

    // Gives element `s` the names of an input element, with their records.
    void inherit(SubId s, const SourceKey& source)
    {
        const ElementMap* map = in_.inputMaps[static_cast<std::size_t>(source.first)];
        for (const Hash256& name : inputNames(source)) {
            const TopoName* record = map->record(name);
            if (record == nullptr || record->kind != s.kind)
                continue; // a map always has the record; a name of another kind would break the map
            names_[slot(s.kind)][s.index].push_back(name);
            records_.push_back(*record);
        }
        sortUnique(names_[slot(s.kind)][s.index]);
    }

    // ---------- rule 1 ----------
    void nameSweepFaces()
    {
        std::vector<Pending> pending;
        for (const kernel::SweepFaceOrigin& origin : in_.sweepOrigins) {
            if (origin.face.kind != SubKind::Face || origin.face.index >= t_.count[slot(SubKind::Face)])
                continue;
            Pending p;
            p.element = origin.face;
            switch (origin.role) {
            case kernel::SweepFaceOrigin::Role::Side:
                p.role = in_.featureRole + ".side";
                p.parents = {detail::curveTagParent(origin.curveTag)};
                break;
            case kernel::SweepFaceOrigin::Role::StartCap:
                p.role = in_.featureRole + ".startcap";
                p.parents = {detail::loopTagParent(origin.loopTag)};
                break;
            case kernel::SweepFaceOrigin::Role::EndCap:
                p.role = in_.featureRole + ".endcap";
                p.parents = {detail::loopTagParent(origin.loopTag)};
                break;
            }
            pending.push_back(std::move(p));
        }
        flush(pending);
    }

    // ---------- rule 2 ----------
    Status indexHistory()
    {
        for (const SubKind kind : kNamedKinds) {
            modifiedFrom_[slot(kind)].resize(t_.count[slot(kind)]);
            generatedFrom_[slot(kind)].resize(t_.count[slot(kind)]);
        }
        if (in_.history == nullptr) {
            if (!in_.inputMaps.empty())
                return tl::unexpected(
                    makeError(ErrorCode::InvalidArgument, "Input element maps were given without a history."));
            return {};
        }
        for (const kernel::HistoryEntry& e : in_.history->entries) {
            if (e.inputIndex < 0 || static_cast<std::size_t>(e.inputIndex) >= in_.inputMaps.size())
                return tl::unexpected(
                    makeError(ErrorCode::InvalidArgument, "A history entry names an input that was not given."));
            if (e.source.kind == SubKind::Solid)
                continue;
            const SourceKey source{e.inputIndex, e.source};
            if (e.relation == kernel::HistoryEntry::Relation::Deleted) {
                modifiedTo_.try_emplace(source);
                continue;
            }
            for (const SubId& r : e.results) {
                if (r.kind == SubKind::Solid)
                    continue;
                if (r.index >= t_.count[slot(r.kind)])
                    return tl::unexpected(makeError(ErrorCode::InvalidArgument,
                                                    "A history entry names an element the result does not have."));
                if (e.relation == kernel::HistoryEntry::Relation::Modified && r.kind == e.source.kind) {
                    modifiedFrom_[slot(r.kind)][r.index].push_back(source);
                    modifiedTo_[source].push_back(r);
                } else {
                    generatedFrom_[slot(r.kind)][r.index].push_back(source);
                }
            }
        }
        for (auto& [source, results] : modifiedTo_) {
            std::sort(results.begin(), results.end());
            results.erase(std::unique(results.begin(), results.end()), results.end());
        }
        for (const SubKind kind : kNamedKinds)
            for (auto* table : {&modifiedFrom_[slot(kind)], &generatedFrom_[slot(kind)]})
                for (std::vector<SourceKey>& sources : *table) {
                    std::sort(sources.begin(), sources.end());
                    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
                }
        return {};
    }

    // Appends every name of result element `s` (none if it has no name yet).
    void addNames(std::vector<Hash256>& out, SubId s) const
    {
        const std::vector<Hash256>& names = names_[slot(s.kind)][s.index];
        out.insert(out.end(), names.begin(), names.end());
    }

    // The face names an edge or vertex name was made from, when its record says so: neighbour names (rule 3) and
    // import names list the faces that contain the element, a generated name from faces only lists those faces.
    // nullopt when the record does not tell (pieces, names from edges or vertices).
    static std::optional<std::vector<Hash256>> facesOfName(const ElementMap& map, const Hash256& name)
    {
        const TopoName* record = map.record(name);
        if (record == nullptr)
            return std::nullopt;
        const std::string& role = record->role;
        if (role == "nb.edge" || role == "nb.vertex" || role == "import.edge" || role == "import.vertex")
            return record->parents;
        const std::size_t g = role.rfind(".g");
        const std::size_t u = role.rfind('_');
        if (g != std::string::npos && u != std::string::npos && u > g && role.compare(g + 2, u - g - 2, "F") == 0)
            return record->parents;
        return std::nullopt;
    }

    // An edge or vertex keeps the names of the input element it was Modified from only while it lies on the faces
    // those names were made from (by name, or as a piece of such a face). An element that history carries onto a
    // different face (for example the start rim of a cut tool that ends on another feature's face) is a different
    // element and is named from its faces instead (rule 3). Faces are not checked: a face is its surface.
    bool facesStillFit(SubId r, const SourceKey& s) const
    {
        if (r.kind == SubKind::Face)
            return true;
        const ElementMap* map = in_.inputMaps[static_cast<std::size_t>(s.first)];
        std::vector<Hash256> around;
        const auto& faces = r.kind == SubKind::Edge ? t_.edgeFaces[r.index] : t_.vertexFaces[r.index];
        for (const std::uint32_t f : faces)
            for (const Hash256& name : names_[slot(SubKind::Face)][f]) {
                around.push_back(name);
                if (const TopoName* record = recordOf(name); record != nullptr && record->role == detail::kSplitRole)
                    around.insert(around.end(), record->parents.begin(), record->parents.end());
            }
        sortUnique(around);
        for (const Hash256& name : inputNames(s)) {
            const auto wanted = facesOfName(*map, name);
            if (!wanted)
                continue;
            for (const Hash256& face : *wanted)
                if (!std::binary_search(around.begin(), around.end(), face))
                    return false;
        }
        return true;
    }

    // The parents of the pieces of input element `s`: every name it holds.
    std::vector<Hash256> splitParents(const SourceKey& s) const
    {
        const std::span<const Hash256> names = inputNames(s);
        std::vector<Hash256> parents(names.begin(), names.end());
        sortUnique(parents);
        return parents;
    }

    std::size_t modifiedCount(const SourceKey& s) const
    {
        const auto it = modifiedTo_.find(s);
        return it != modifiedTo_.end() ? it->second.size() : 0;
    }

    void nameFromHistory(SubKind kind)
    {
        std::vector<Pending> pending;
        for (std::uint32_t i = 0; i < t_.count[slot(kind)]; ++i) {
            const SubId r{kind, i};
            if (named(r))
                continue;
            const std::vector<SourceKey>& modified = modifiedFrom_[slot(kind)][i];
            const std::vector<SourceKey>& generated = generatedFrom_[slot(kind)][i];
            bool fromModified = false;
            for (const SourceKey& s : modified) {
                const std::span<const Hash256> source = inputNames(s);
                if (source.empty() || !facesStillFit(r, s))
                    continue;
                fromModified = true;
                if (modifiedCount(s) == 1) {
                    inherit(r, s);
                } else {
                    Pending p;
                    p.element = r;
                    p.role = std::string(detail::kSplitRole);
                    p.parents = splitParents(s);
                    p.sourceInput = s.first;
                    p.source = s.second;
                    pending.push_back(std::move(p));
                }
            }
            if (fromModified || generated.empty())
                continue;
            std::vector<Hash256> parents;
            std::string kinds;
            for (const SourceKey& s : generated) {
                const std::span<const Hash256> source = inputNames(s);
                if (source.empty())
                    continue;
                parents.insert(parents.end(), source.begin(), source.end());
                const char letter = kindLetter(s.second.kind);
                if (kinds.find(letter) == std::string::npos)
                    kinds.push_back(letter);
            }
            if (parents.empty())
                continue; // named from its neighbours (rule 3)
            std::sort(kinds.begin(), kinds.end());
            // An edge or vertex generated from faces alone is where those faces meet, which is what a neighbour
            // name says: it is left to rule 3, so that the pieces of one meeting line are siblings of one family
            // whichever way OCCT reported them (one piece with history, the next without).
            if (kinds == "F" && kind != SubKind::Face)
                continue;
            sortUnique(parents);
            Pending p;
            p.element = r;
            p.role = in_.featureRole + ".g" + kinds + "_" + kindLetter(kind);
            p.parents = std::move(parents);
            pending.push_back(std::move(p));
        }
        flush(pending);
    }

    // ---------- rule 3 ----------
    void nameFromNeighbours()
    {
        std::vector<Pending> pending;
        for (std::uint32_t i = 0; i < t_.count[slot(SubKind::Face)]; ++i) {
            const SubId face{SubKind::Face, i};
            if (named(face))
                continue;
            Pending p;
            p.element = face;
            p.role = "nb.face";
            for (const std::uint32_t e : t_.faceEdges[i])
                addNames(p.parents, {SubKind::Edge, e});
            if (p.parents.empty()) {
                p.role = "nb.facef";
                for (const std::uint32_t f : t_.faceNeighbours[i])
                    if (f != i)
                        addNames(p.parents, {SubKind::Face, f});
            }
            if (p.parents.empty())
                continue;
            sortUnique(p.parents);
            pending.push_back(std::move(p));
        }
        flush(pending);
        for (const SubKind kind : {SubKind::Edge, SubKind::Vertex}) {
            for (std::uint32_t i = 0; i < t_.count[slot(kind)]; ++i) {
                const SubId element{kind, i};
                if (named(element))
                    continue;
                Pending p;
                p.element = element;
                p.role = kind == SubKind::Edge ? "nb.edge" : "nb.vertex";
                const auto& faces = kind == SubKind::Edge ? t_.edgeFaces[i] : t_.vertexFaces[i];
                for (const std::uint32_t f : faces)
                    addNames(p.parents, {SubKind::Face, f});
                if (p.parents.empty())
                    continue;
                sortUnique(p.parents);
                pending.push_back(std::move(p));
            }
            flush(pending);
        }
    }

    // ---------- rule 4 ----------
    // The faces around an element that are not its own faces, by SubId index, sorted.
    std::vector<std::uint32_t> ring(SubId s) const
    {
        std::vector<std::uint32_t> faces;
        std::vector<std::uint32_t> own;
        switch (s.kind) {
        case SubKind::Face:
            faces = t_.faceNeighbours[s.index];
            own = {s.index};
            break;
        case SubKind::Edge:
            own = t_.edgeFaces[s.index];
            for (const std::uint32_t v : t_.edgeVertices[s.index])
                faces.insert(faces.end(), t_.vertexFaces[v].begin(), t_.vertexFaces[v].end());
            break;
        case SubKind::Vertex:
            own = t_.vertexFaces[s.index];
            for (const std::uint32_t e : t_.vertexEdges[s.index])
                for (const std::uint32_t v : t_.edgeVertices[e])
                    if (v != s.index)
                        faces.insert(faces.end(), t_.vertexFaces[v].begin(), t_.vertexFaces[v].end());
            break;
        case SubKind::Solid:
            break;
        }
        sortUnique(faces);
        sortUnique(own);
        std::vector<std::uint32_t> out;
        std::set_difference(faces.begin(), faces.end(), own.begin(), own.end(), std::back_inserter(out));
        return out;
    }

    // Discriminator of `p`: the names of the faces around it, minus its parents; with `currentFeatureOnly`, only
    // names made by this feature. A face that is a piece of a split face counts by the names it was split from:
    // which face bounds a sibling should not change because that face was later cut in two somewhere else.
    std::vector<Hash256> discriminator(const Pending& p, bool currentFeatureOnly) const
    {
        std::vector<Hash256> out;
        for (const std::uint32_t f : ring(p.element)) {
            for (const Hash256& name : names_[slot(SubKind::Face)][f]) {
                const TopoName* record = recordOf(name);
                if (currentFeatureOnly && (record == nullptr || record->origin != in_.feature))
                    continue;
                if (record != nullptr && record->role == detail::kSplitRole)
                    out.insert(out.end(), record->parents.begin(), record->parents.end());
                else
                    out.push_back(name);
            }
        }
        sortUnique(out);
        std::vector<Hash256> result;
        std::set_difference(out.begin(), out.end(), p.parents.begin(), p.parents.end(), std::back_inserter(result));
        return result;
    }

    const TopoName* recordOf(const Hash256& name) const
    {
        const auto it = std::lower_bound(sortedRecords_.begin(), sortedRecords_.end(), name,
                                         [](const TopoName& r, const Hash256& h) { return r.id < h; });
        if (it != sortedRecords_.end() && it->id == name)
            return &*it;
        // Records added since the last sort (none are looked up in practice: discriminators read face names, and
        // flush() sorts before it computes them).
        for (const TopoName& r : records_)
            if (r.id == name)
                return &r;
        return nullptr;
    }

    void issue(const Pending& p, std::vector<Hash256> discriminator, std::uint32_t ordinal, std::uint32_t siblings)
    {
        TopoName name{};
        name.kind = p.element.kind;
        name.origin = in_.feature;
        name.role = p.role;
        name.parents = p.parents;
        name.discriminator = std::move(discriminator);
        name.ordinal = ordinal;
        name.siblingCount = siblings;
        name.family = p.family;
        name.id = detail::nameId(name);
        names_[slot(p.element.kind)][p.element.index].push_back(name.id);
        if (p.sourceInput >= 0)
            splitChildren_[{p.sourceInput, p.source}].push_back(name.id);
        records_.push_back(std::move(name));
    }

    void flush(std::vector<Pending>& pending)
    {
        if (pending.empty()) {
            sortRecords(); // inherited records become visible to recordOf()
            return;
        }
        for (Pending& p : pending)
            p.family = detail::familyId(p.element.kind, in_.feature, p.role, p.parents);
        std::vector<std::size_t> order(pending.size());
        for (std::size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        // Family, then element: the element order only makes the sort total.
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            if (pending[a].family != pending[b].family)
                return pending[a].family < pending[b].family;
            return pending[a].element < pending[b].element;
        });
        // Names alone in their family first: their names are final and may serve as discriminators below.
        std::vector<std::pair<std::size_t, std::size_t>> groups; // [begin, end) in `order`
        for (std::size_t i = 0; i < order.size();) {
            std::size_t end = i + 1;
            while (end < order.size() && pending[order[end]].family == pending[order[i]].family)
                ++end;
            if (end - i == 1)
                issue(pending[order[i]], {}, 0, 1);
            else
                groups.emplace_back(i, end);
            i = end;
        }
        refreshNames(pending);
        sortRecords();

        for (const auto& [begin, end] : groups) {
            std::vector<std::size_t> members(order.begin() + static_cast<std::ptrdiff_t>(begin),
                                             order.begin() + static_cast<std::ptrdiff_t>(end));
            std::vector<std::vector<Hash256>> discs(members.size());
            for (const bool currentOnly : {true, false}) {
                std::set<std::vector<Hash256>> seen;
                bool distinct = true;
                for (std::size_t k = 0; k < members.size(); ++k) {
                    discs[k] = discriminator(pending[members[k]], currentOnly);
                    if (discs[k].empty() || !seen.insert(discs[k]).second)
                        distinct = false;
                }
                if (!currentOnly) {
                    // The fallback mode is marked, so that its discriminators never equal those of the first mode.
                    const Hash256 mark = detail::allFacesDiscriminatorMark();
                    for (std::vector<Hash256>& d : discs)
                        d.insert(std::lower_bound(d.begin(), d.end(), mark), mark);
                }
                if (distinct)
                    break;
            }
            // Sub-groups by discriminator; an ordinal (by fingerprint) only inside a sub-group of several.
            std::vector<std::size_t> idx(members.size());
            for (std::size_t k = 0; k < idx.size(); ++k)
                idx[k] = k;
            auto fingerprintOf = [&](std::size_t k) -> const Fingerprint& {
                const SubId e = pending[members[k]].element;
                return t_.fingerprint[slot(e.kind)][e.index];
            };
            std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
                if (discs[a] != discs[b])
                    return discs[a] < discs[b];
                if (const auto c = detail::ordinalOrder(fingerprintOf(a), fingerprintOf(b)); c != 0)
                    return c < 0;
                return pending[members[a]].element < pending[members[b]].element;
            });
            const auto siblings = static_cast<std::uint32_t>(members.size()); // the family size
            for (std::size_t i = 0; i < idx.size();) {
                std::size_t end2 = i + 1;
                while (end2 < idx.size() && discs[idx[end2]] == discs[idx[i]])
                    ++end2;
                std::uint32_t ordinal = end2 - i > 1 ? 1 : 0;
                for (std::size_t k = i; k < end2; ++k) {
                    // Equal fingerprints share an ordinal and so a name: find() then returns neither element.
                    if (k > i && detail::ordinalOrder(fingerprintOf(idx[k - 1]), fingerprintOf(idx[k])) != 0)
                        ++ordinal;
                    issue(pending[members[idx[k]]], discs[idx[k]], ordinal, siblings);
                }
                i = end2;
            }
        }
        refreshNames(pending);
        sortRecords();
        pending.clear();
    }

    void refreshNames(const std::vector<Pending>& pending)
    {
        for (const Pending& p : pending)
            sortUnique(names_[slot(p.element.kind)][p.element.index]);
    }

    void sortRecords()
    {
        sortedRecords_.insert(sortedRecords_.end(), records_.begin(), records_.end());
        records_.clear();
        std::sort(sortedRecords_.begin(), sortedRecords_.end(),
                  [](const TopoName& a, const TopoName& b) { return a.id < b.id; });
        sortedRecords_.erase(std::unique(sortedRecords_.begin(), sortedRecords_.end(),
                                         [](const TopoName& a, const TopoName& b) { return a.id == b.id; }),
                             sortedRecords_.end());
    }

    // ---------- lineage ----------
    std::vector<LineageEvent> lineage() const
    {
        std::vector<LineageEvent> out;
        // Results that several inputs were Modified into (1:1 each): their names, by result.
        for (std::size_t input = 0; input < in_.inputMaps.size(); ++input) {
            const ElementMap* map = in_.inputMaps[input];
            if (map == nullptr || !map->impl)
                continue;
            for (const ElementMap::Impl::Element& element : map->impl->elements) {
                if (element.sub.kind == SubKind::Solid)
                    continue;
                const SourceKey source{static_cast<int>(input), element.sub};
                const auto it = modifiedTo_.find(source);
                if (it == modifiedTo_.end() || it->second.empty()) {
                    for (const Hash256& name : element.names)
                        out.push_back(LineageEvent{LineageEvent::Kind::Deleted, name, {}, in_.feature});
                    continue;
                }
                if (it->second.size() > 1) {
                    std::vector<Hash256> pieces;
                    if (const auto c = splitChildren_.find(source); c != splitChildren_.end())
                        pieces = c->second;
                    sortUnique(pieces);
                    for (const Hash256& name : element.names)
                        out.push_back(LineageEvent{LineageEvent::Kind::Split, name, pieces, in_.feature});
                    continue;
                }
                const SubId result = it->second.front();
                std::size_t sources = 0;
                for (const SourceKey& s : modifiedFrom_[slot(result.kind)][result.index])
                    if (modifiedCount(s) == 1 && !inputNames(s).empty())
                        ++sources;
                if (sources > 1)
                    for (const Hash256& name : element.names)
                        out.push_back(LineageEvent{LineageEvent::Kind::Merged, name,
                                                   names_[slot(result.kind)][result.index], in_.feature});
            }
        }
        return out;
    }

    // ---------- the map ----------
    Result<ElementMap> finish()
    {
        sortRecords();
        ElementMap::Impl table;
        std::vector<Hash256> held;
        for (const SubKind kind : {SubKind::Vertex, SubKind::Edge, SubKind::Face}) { // SubId order
            for (std::uint32_t i = 0; i < t_.count[slot(kind)]; ++i) {
                const std::vector<Hash256>& names = names_[slot(kind)][i];
                if (names.empty())
                    continue;
                table.elements.push_back({SubId{kind, i}, t_.fingerprint[slot(kind)][i], names});
                held.insert(held.end(), names.begin(), names.end());
            }
        }
        sortUnique(held);
        for (const TopoName& record : sortedRecords_)
            if (std::binary_search(held.begin(), held.end(), record.id))
                table.records.push_back(record);
        return detail::finishMap(std::move(table), false);
    }

    const NamingInput& in_;
    Topology t_;
    std::array<std::vector<std::vector<Hash256>>, 3> names_; // by slot, by index; sorted, unique
    std::vector<TopoName> records_;                          // issued or inherited since the last sortRecords()
    std::vector<TopoName> sortedRecords_;                    // sorted by id, unique
    std::array<std::vector<std::vector<SourceKey>>, 3> modifiedFrom_, generatedFrom_; // by result
    std::map<SourceKey, std::vector<SubId>> modifiedTo_;      // by input element; empty = Deleted
    std::map<SourceKey, std::vector<Hash256>> splitChildren_; // names of the pieces of a split input element
};

Result<NamingOutput> propagateChecked(const NamingInput& in)
{
    if (in.result == nullptr || in.result->isNull())
        return tl::unexpected(makeError(ErrorCode::InvalidArgument, "There is no result shape to name."));
    for (const ElementMap* map : in.inputMaps)
        if (map == nullptr)
            return tl::unexpected(makeError(ErrorCode::InvalidArgument, "An input element map is missing."));
    Propagator propagator(in, gather(*in.result));
    return propagator.run();
}

} // namespace

Result<NamingOutput> propagate(const NamingInput& in)
{
    try {
        return propagateChecked(in);
    } catch (const std::bad_alloc&) {
        return tl::unexpected(makeError(ErrorCode::Internal, "Out of memory while naming the result."));
    } catch (...) {
        return tl::unexpected(makeError(ErrorCode::Internal, "Unexpected failure while naming the result."));
    }
}

} // namespace ray::naming
