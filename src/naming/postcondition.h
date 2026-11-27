// Private to src/naming and its tests (T-081b round 8): the resolver's post-condition, and the pick-time samples it
// checks every answer against (naming.h PickSamples).
//
// THE POST-CONDITION. Every answer the resolver would give that is not Broken (Exact, Split, Merged; Repaired is a
// suggestion and is left alone) is checked last, for faces, edges and vertices alike, whatever found it (a name, a
// family, a fingerprint), with no early return:
//  - The move. Each surface the pick lay on (PickSamples::carriers) moves with the feature that owns it (a sweep's side
//    or cap, a hole's wall or floor; a piece of a split face, the owner of the face it was split from): by that
//    feature's placement then and now when it changed, else not at all. The samples are mapped so: on surfaces that all
//    moved, rigidly; where a moved surface meets one that stayed (an edge or vertex where a moved wall crosses a face
//    that did not move), carried with the moved one, slid along the moved feature's direction (its frame's z, or within
//    the moved surface when z runs along the other one) onto the one that stayed, and settled onto all of them (a few
//    Gauss-Newton steps): the crossing of the moved surface with the stationary one, not a rigid move of the point. A
//    crossing whose surfaces all lie where they did (those that moved moved along themselves) stays where it was,
//    unless a face at one of its ends was moved off itself, it is an edge, and it lies on no cap of the moved feature
//    and on no imported face (the oracle's map of such crossings). The move is KNOWN when some surface has an owner,
//    every owner still has a placement, and those that moved moved alike; otherwise (only surfaces without an owner: an
//    imported face, a direct edit's; an owner gone; a reference without samples, whose territory points stand in) the
//    samples stay where they were. A piece split off by a feature whose own face it may not be (its parents unknown) is
//    checked both ways: a Merged answer as moved with that feature, any other first in place.
//  - Where a mapped sample lands: on each element of the pick's kind that holds it on the pick's mapped surfaces (a
//    face on the same plane on the same side, or the same cylinder the same way round; an edge running along the mapped
//    crossing with a face on each mapped surface, and at an edge's end one that runs from there the way the pick did;
//    a vertex with a face on each mapped surface but one).
//  - P1 (holds the pick): a sample that lands on some element lands on the answer. Samples that land on nothing (the
//    ground was cut away) are not required. A picked end on the answer that runs from it the other way: Broken.
//  - P3 (complete): samples that land on other faces: Split with those too when the move is known, the answer holds
//    some of the samples and they all lie on the answer's carrier; an edge or vertex, or else: Broken.
//  - Every member of a Split or Merged answer holds part of the mapped pick: a mapped sample, or one of its own
//    territory points among the mapped samples (within PickSamples::qSpacing of one, on the mapped surface; an edge not
//    past the mapped ends; a planar face inside the picked outline as moved). An edge piece of a Split wholly past the
//    pick as its feature moved it is left out; any other member that holds none makes the answer Broken.
//  - P2 (holds nothing else's): an Exact answer on which a recorded neighbour's territory lands, where it was or as its
//    own feature moved it (PickSamples::neighbourOwners), is Merged (Broken for a vertex).
//  - An answer that holds no mapped sample, nothing of them landing anywhere nor within 0.05 mm of another element: an
//    Exact one whose feature's placement changed is checked again with the samples where they were picked (a placement
//    is the centre of its feature's profile or points, so a dimension edit places all of the feature elsewhere), and
//    stands when it holds them there. Otherwise it must be on ground no recorded neighbour held, and stands only (a) on
//    the pick's mapped surfaces with the move known, as the pick bounded elsewhere by the part (a face: moved, a rigid
//    copy of the pick; an edge reaching into the mapped pick's place; never a vertex), after the same checks in place
//    when the pick's feature moved along the pick's own surface; or (b) as the pick moved along its normal: the samples
//    moved onto its surface (a plane along its normal, a cylinder out from its axis) or its curve land on it (a face at
//    least half of them, an edge one or more while its faces lie on the picked surfaces or parallel ones; a vertex
//    where the pick's surfaces meet but one, that one moved along its normal); or (c) a face, as a rigid copy of the
//    pick (a body moved onto a mate). (b) keeps the answer when the face's surface is the picked one so moved (a
//    parallel plane, a parallel cylinder overlapping it) or for an edge or vertex; else it is flagged Merged.
// A reference with neither samples nor territory resolves as before (nothing to check). Never throws.
#pragma once
#include "territory.h"

namespace ray::naming::detail {

// The samples of `sub` (naming.h PickSamples), with the owners of the pick's surfaces and of `neighbours` (as
// pickNeighboursOf made them). Version 0 (none) when nothing could be sampled. Deterministic; never throws.
PickSamples pickSamplesOf(const ElementMap& map, const TerritoryCache& territories, kernel::SubId sub,
                          const std::vector<PickNeighbour>& neighbours);

// The post-condition on answer `r` to `ref` in the shape `territories` was made for (with `map`, its element map).
Resolved postcondition(const ElementMap& map, const TerritoryCache& territories, const TopoRef& ref, Resolved r);

} // namespace ray::naming::detail
