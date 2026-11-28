// Private to src/naming and its tests (T-081b round 8): the resolver's post-condition, and the pick-time samples it
// checks every answer against (naming.h PickSamples).
//
// GENERATOR IDENTITY (T-081b round 9) comes first: an Exact answer to a whole generator element (naming.h
// PickSamples::whole; a reference without samples or with version-1 samples: its one name pure lineage and held by one
// element, read from the current map), whose name leads to that one element, whole again, every generator of its
// lineage still there, IS the pick: only P2 below is asked (Merged when it holds a neighbour's ground), and the answer
// carries Resolved::identity. Except where the answer still lies on the pick's own surfaces, every point of the pick
// (where picked, or as its features moved it) on every surface the answer lies on: there P1 is computable (whether the
// answer holds the pick on those surfaces), and the post-condition below decides as for any answer (a whole boss side
// whose visible patch another feature moved is another patch, holdout1 7:33 face 18), but for one thing it could only
// guess: an answer that holds none of the pick as moved but most of it where it was picked is the pick (a placement
// changed by a dimension edit of its feature is no move). And never where the pick, as its features moved it, lands on
// another element of its kind (that one holds it). Identity decides where the generator reshaped its element (a cap
// pushed by a new depth, a side turned with its sketch line, the edge between them). A piece of a split face is never a
// whole generator element. A REFERENCE WITHOUT SAMPLES
// (T-081b round 9) is Exact only so: a Split or Merged answer to it is Broken, any other answer is checked against the
// territory it has and is at most Repaired. A GUESS NEVER KEEPS EXACT (T-081b round 9): every rule below that keeps an
// answer whose element holds nothing of the pick as its features moved it (checked in place after a placement change,
// the pick moved along its normal, a rigid copy of it, and since T-081b round 10 the crossing nearest where the pick
// was) gives Repaired, a suggestion the user confirms, instead of Exact.
//
// Identity's three refusals (postcondition.cpp refusedIdentity, each a product rule, each pinned) and DERIVED IDENTITY
// (T-081b round 10, naming.h Resolved::derived: an edge or vertex that was the only element between the faces it is
// named from, those faces still theirs by name and one element between them now) are written down at postcondition().
// FACE MOVES (T-081b round 10, NamingInput::faceMoves): a direct edit's move of a face it was given carries what lies
// on that face like a feature's placement, composed after the placement and the body moves (below, "the move").
//
// THE POST-CONDITION. Every answer the resolver would give that is not Broken (Exact, Split, Merged; Repaired is a
// suggestion and is left alone) is checked last, for faces, edges and vertices alike, whatever found it (a name, a
// family, a fingerprint), with no early return:
//  - The move. Each surface the pick lay on (PickSamples::carriers) moves with the feature that owns it (a sweep's side
//    or cap, a hole's wall or floor; a piece of a split face, the owner of the face it was split from, known at pick
//    time from the split records' parents the map keeps as ancestors): by that feature's placement then and now when
//    it changed, and by the body moves (NamingInput::bodyPlacement) that carry its ground (PickSamples::grounds) then
//    and now, and by the face moves (NamingInput::faceMoves) that carry its face (PickSamples::carrierNames, version
//    3) then and now, composed; else not at all. The samples are mapped so: on surfaces that all
//    moved, rigidly; where a moved surface meets one that stayed (an edge or vertex where a moved wall crosses a face
//    that did not move), carried with the moved one, slid along the moved feature's direction (its frame's z, or within
//    the moved surface when z runs along the other one) onto the one that stayed, and settled onto all of them (a few
//    Gauss-Newton steps): the crossing of the moved surface with the stationary one, not a rigid move of the point. A
//    crossing whose surfaces all lie where they did (those that moved moved along themselves) stays where it was: its
//    surfaces fix its line and its end faces its ends; unless a face at one of its ends was moved off itself by the
//    edge's feature, which then bounds it elsewhere along the line, and it lies on no cap of the moved feature and on
//    no face nothing places (then it is the boundary of material that did not move, which stays). The move is KNOWN
//    when some surface has an owner or a body move, every owner still has a placement, and those that moved moved
//    alike; otherwise (only surfaces without an owner: an imported face, a direct edit's; an owner gone; a reference
//    without samples, whose territory points stand in) the samples stay where they were. A piece split off by a feature
//    whose own face it may not be (its parents unknown) is checked both ways: a Merged answer as moved with that
//    feature, any other first in place.
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
//    is suggested (Repaired) when it holds them there. Otherwise it must be on ground no recorded neighbour held, and
//    is suggested (an Exact one; Repaired) or kept (Split, Merged) only (a) on
//    the pick's mapped surfaces with the move known, as the pick bounded elsewhere by the part (a face: moved, a rigid
//    copy of the pick; an edge reaching into the mapped pick's place; never a vertex), after the same checks in place
//    when the pick's feature moved along the pick's own surface; or (b) as the pick moved along its normal: the samples
//    moved onto its surface (a plane along its normal, a cylinder out from its axis) or its curve land on it (a face at
//    least half of them, an edge one or more while its faces lie on the picked surfaces or parallel ones; a vertex
//    where the pick's surfaces meet but one, that one moved along its normal); or (c) a face, as a rigid copy of the
//    pick (a body moved by a move nothing recorded). (b) suggests the answer when the face's surface is the picked one
//    so moved (a parallel plane, a parallel cylinder overlapping it) or for an edge or vertex; else it is flagged
//    Merged.
//  - P3 is checked on faces only. An edge's mapped samples that land on other edges are not made a Split: an edge is a
//    stretch of a crossing, and where the pick's mapped place runs past the answer along the same crossing the next
//    edge there may be the continuation of ANOTHER pick-time edge of the same crossing, moved with it to meet the
//    pick's place (T-081b round 8, flange 6:3 edge 10: a Split with the next stretch took in the other stretch's
//    ground, which the pick's samples, all on the crossing, cannot tell from its own). Such an answer is Broken; the
//    completeness of an edge's Split is completeAgainstEvidence's (territory.h), against the pick's territory and its
//    recorded neighbours, before the post-condition.
// Never throws.
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
