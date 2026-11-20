// Private to src/naming and its tests: pick-time evidence (naming.h Territory, PickNeighbour) and the geometric tests
// the resolver and propagate() run against it.
//
// A territory is made from the exact geometry, without a mesh (kernel queries only), deterministically:
//  - an edge: its box (exact for a line; from 129 points along an arc; from its ends and the points below for any
//    other curve), its length, and points at fixed fractions of its length (0.5, 0.3, 0.7, 0.15, 0.85), each at least
//    kTerritoryMargin from both ends (another curve: the feet on it of those points of its chord);
//  - a face: its box (SurfaceInfo::bounds), its area, and points on it at least kTerritoryMargin from every edge of
//    it: for a plane bounded by lines and arcs, the cell centres of a grid over its outline that lie inside it; for a
//    cylinder bounded by lines along it and circles about it, the same over (angle, height); either way chosen
//    farthest-first, starting from the one furthest from the boundary. Any other face: the feet on it of its centroid
//    and a 2 x 2 x 2 grid over its box (straight out from the axis or centre on a cylinder or sphere, kept when the
//    kernel puts them on the face; else the kernel's nearest point), the first kTerritoryPoints far enough from every
//    edge;
//  - a vertex: its point.
// Probe tells whether a point lies on an element of a (later) shape: analytic for vertices, lines, arcs, planes and
// cylinders bounded as above (arcs walked in chords within 1e-5 mm of the arc), else off the carrier surface (plane,
// cylinder, sphere) analytically and on it by kernel::minDistance to a small box around the point.
#pragma once
#include "naming_internal.h"

#include <map>
#include <memory>
#include <mutex>
#include <optional>

namespace ray::naming::detail {

// How far inside its element (from its boundary) a territory point lies, at least, mm.
inline constexpr double kTerritoryMargin = 2e-3;
// How near a point must be to an element to lie on it, mm.
inline constexpr double kOnTolerance = 1e-3;

Vec3 fromQuanta(const std::array<std::int64_t, 3>& q);
std::array<std::int64_t, 3> toQuanta(const Vec3& p);
// The box of a territory, mm.
Box3 boxOf(const Territory& territory);
Vec3 centreOf(const Box3& box);

// The territory of `sub` (naming.h). Points empty when none were found; never throws.
Territory territoryOf(const kernel::Shape& shape, kernel::SubId sub);

// Point-on-element tests against one element of one shape. Never throws; a test that cannot be made is false.
class Probe {
public:
    Probe(const kernel::Shape& shape, kernel::SubId sub);
    ~Probe();
    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;
    bool on(const Vec3& p, double tol = kOnTolerance) const;
    // How many of the territory's points lie on the element.
    std::size_t count(const Territory& territory) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Territories and carrier data of one shape's elements, made once each; thread-safe.
class TerritoryCache {
public:
    explicit TerritoryCache(const kernel::Shape& shape);
    ~TerritoryCache();
    const Territory& of(kernel::SubId sub) const;
    // Do faces (or edges) a and b lie on one carrier: the same plane with the same outward side, the same cylinder,
    // the same line, the same circle?
    bool sameCarrier(kernel::SubId a, kernel::SubId b) const;
    // A quantised description of a face's plane or cylinder (empty for any other surface): faces with equal keys lie
    // on one carrier.
    std::string carrierKey(kernel::SubId face) const;

private:
    struct Carrier;
    const Carrier& carrier(kernel::SubId sub) const;
    const kernel::Shape& shape_;
    mutable std::mutex lock_;
    mutable std::map<kernel::SubId, Territory> made_;
    mutable std::map<kernel::SubId, std::unique_ptr<Carrier>> carriers_;
};

// The other elements recorded with a reference to `sub` (naming.h PickNeighbour): siblings (a shared name family
// or split parent; for an edge or vertex, an element of the same kind on exactly the faces of the picked one), then
// faces or edges on the same carrier; at most kMaxPickNeighbours: up to 5 siblings, then carrier elements, then more
// siblings, each group nearest first (gap between boxes, then SubId). Elements without a name are left out. Never
// throws.
std::vector<PickNeighbour> pickNeighboursOf(const ElementMap& map, const kernel::Shape& shape, kernel::SubId sub,
                                            const TerritoryCache& territories);

// The box of element `sub` as a territory would record it (no points made). nullopt when unreadable.
std::optional<Box3> elementBox(const kernel::Shape& shape, kernel::SubId sub);

// ---------------------------------------------------------------- the resolver's evidence rules (naming.h TopoRef)
// The completeness check: is a name-based answer (`answer`: the Exact element, the Split pieces, the Merged element)
// all of what was picked? nullopt when the evidence does not object (or there is none, or the reference is to a
// vertex). The picked element's place is known while the answer still covers some of its own territory (the picked
// element's box at pick time); otherwise the answer moved and its place now is not known. An element of the answer's
// kind outside the answer holds part of what was picked when
//  - (by territory, holdout4 9:9) the place is known and it covers a point of the picked element's own territory that
//    the answer no longer covers, on the answer's carrier (`territories`: plane on the same side, cylinder, line,
//    circle), an edge also between the answer's surfaces (as in the continuation test below: a stretch of the picked
//    line that now lies between other surfaces is where the pick was, not what was picked). Points on nothing are
//    territory cut away, which an answer may lose;
//  - (by continuation) it continues an answer element with nothing geometric between them: shares a vertex (an edge)
//    or an edge (a face) with it, on the same carrier, an edge also with each of its faces equal to or on the carrier
//    of one of the answer element's, and
//      - (holdout5 1:2) the place is known, it reaches into the place (overlaps the picked box along every axis it
//        extends along), and it was not apart from the pick at pick time (it carries no name the pick or a recorded
//        neighbour had, and covers no point of a recorded neighbour's territory: a face's box is a coarse stand-in for
//        its place, and the boundary between the pick and a neighbour may move a little): the answer stopped short of
//        where the pick ran, and the rest is under another name;
//      - (holdout4 5:9) the place is not known and no recorded neighbour touched the picked element at pick time
//        (boxes within the extent tolerance; a neighbour without a known box counts as touching): which stretch of the
//        run is the moved pick cannot be told. When a recorded neighbour touched it, the pick was already one stretch
//        of a longer run (the two arcs of one hole's rim), and the run may simply have moved with it.
// When some element holds part of the pick: Split with the fuller set when it can be shown to be all of the pick (edges
// on a line or a circle with a known place only: every edge on the answer's line or circle reaching into the picked
// box is in it, each added one inside the picked segment, or arc as the hint and the circle tell it, between the
// answer's surfaces, under no name the pick or a recorded neighbour had, and the set fits the evidence as Split pieces
// must, piecesFitEvidence); else Broken.
std::optional<Resolved> completeAgainstEvidence(const ElementMap& map, const kernel::Shape& shape,
                                                const TerritoryCache& territories,
                                                const std::vector<kernel::SubId>& answer, const TopoRef& ref);

// The Exact stage's answer `element` against the reference's evidence; nullopt when the evidence does not object (or
// there is none, or the element is unchanged: its fingerprint matches the hint). Otherwise, the first that applies:
//  - the element covers a point of a recorded neighbour's territory: Merged when it also still covers a point of the
//    picked element's own territory (it took the neighbour in), else Broken (it lies over the neighbour's place and
//    not where it was picked); a vertex is never Merged;
//  - the completeness check (completeAgainstEvidence) on {element} objects: its answer (Split with the fuller set, or
//    Broken). Otherwise, covering some of its own territory, the element is not objected to;
//  - the element covers none of its own territory (it moved, or what was picked of it is gone), grew, and every name of
//    a recorded sibling is gone from the map: the pieces may have joined where it is now: Merged (not a vertex);
//  - the element covers none of its own territory and a recorded sibling's names lead to one element now whose shift
//    since the pick (box centre to box centre) differs from the element's by more than the tolerance: the pieces did
//    not move together, and which is which cannot be told: Broken.
// A reference without evidence (an older document) skips all of these.
std::optional<Resolved> exactAgainstEvidence(const ElementMap& map, const kernel::Shape& shape,
                                             const TerritoryCache& territories, kernel::SubId element,
                                             const TopoRef& ref);
// May `pieces` be answered Split against the reference's evidence? No (with the reason) when together they do not fit
// in the picked element's box, translated (a split adds no territory, a move keeps the size), or one of them covers a
// point of a recorded neighbour's territory. Yes when there is no evidence.
bool piecesFitEvidence(const ElementMap& map, const kernel::Shape& shape, const std::vector<kernel::SubId>& pieces,
                       const TopoRef& ref, std::string& why);

} // namespace ray::naming::detail
