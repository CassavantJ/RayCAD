// Private to src/naming and its tests (task T-081a): how propagate() names the result of a modelling operation,
// and the resolver stages that follow names through the current build (Split, Merged, Repaired).
//
// NAMING RULES (PLAN.md 3.3, with the amendments of spike S4, spikes/s4-naming/REPORT.md section 10). One call of
// propagate() names every face, edge and vertex of `result` (solids are not named), in this order:
//  1. Sweep faces (rule 1). Each SweepFaceOrigin gives its face (feature, featureRole + ".side" | ".startcap" |
//     ".endcap", [hash of the curve tag] or [hash of the loop tag]).
//  2. History (rule 2), faces first, then edges, then vertices:
//     - Modified, one input element to exactly one result: the result keeps every name of the input. A result
//       that several inputs were Modified into (same-domain unification) carries every name of all of them.
//     - Modified, one input element to several results (a split): each piece gets (feature, "split", [every name
//       of the input]); the pieces are siblings of one family.
//     - An edge or vertex takes nothing from a Modified input whose name was made from faces (rule 3, imports,
//       generated from faces) when it no longer lies on all of those faces (by name, or on a piece of one): history
//       can carry an edge onto a different face (the start rim of a cut tool that ends on another feature's
//       face), and such an edge is a different element. It is named by the next rule that applies.
//     - Absorbed (only with NamingInput::inputShapes): a face or edge Modified from one input element only, that
//       grew, and on which every territory point (territory.h) of an input element of its kind that history reports
//       Deleted lies, also carries that element's names (lineage: Merged into it): a boolean unified the two without
//       saying so, and a reference to either must not resolve Exact.
//     - Generated only: (feature, featureRole + ".g" + source kinds + "_" + result kind, [every name of every
//       source]), e.g. "fillet.gE_F" for a blend face, "fillet.gEF_V" for a vertex made from an edge and a face.
//       An edge or vertex generated from faces only is left to rule 3: it is where those faces meet, and two
//       pieces of one meeting line, one reported with history and the next without, must be siblings of one
//       family.
//  3. Elements still without a name (rule 3): a face from the names of its named edges ("nb.face"), or of its
//     neighbouring faces ("nb.facef"); an edge or vertex from the names of the faces that contain it ("nb.edge",
//     "nb.vertex"). OCCT gives the boundary edges and vertices of a blend no ancestor, and the edges and vertices
//     of a sweep have none at all, so this is common, not exceptional.
//  4. Siblings (rule 4): new names that share a family (kind, feature, role, parents) are told apart by a
//     discriminator, the sorted names of the faces around the element that are not among its own faces or its
//     parents (for a face: its neighbours; for an edge: the faces at its end vertices; for a vertex: the faces at
//     the far ends of its edges). Names made by the current feature are tried first, then all names.
//     A discriminator made from all names carries allFacesDiscriminatorMark (naming_internal.h), so it never equals
//     one made from the current feature's names. The only piece of a split element that gets a piece name while
//     others do not (they left the element's faces) is issued with the number of pieces as its sibling count and that
//     mark: which piece it is is not said by the name.
//     Only siblings with identical discriminators get an ordinal (1-based, by fingerprint, ties share it). The
//     sibling count, hashed into the name, is the size of the family when it was issued (as nameImported does), so
//     a resolver can tell that a family it finds is incomplete: a member that was later split or deleted is
//     missing, and its pieces cannot be found from the family (a map keeps only the records of names its elements
//     hold). A name alone in its family has no discriminator, no ordinal and a sibling count of 1.
// Wherever a name is derived from other elements, ALL their names are used, never only the first: a face that
// unification merged holds the name of each face it was made of, and which of them sorts first is arbitrary, so a
// name built from the first could match a different element in another build. The primary (first) name of an
// element is the smallest of its names. Every element is fingerprinted (the same
// fingerprint nameImported gives), so a reference can carry a hint. Nothing depends on the order in which the
// kernel lists sub-shapes beyond SubId order itself, on pointer values, on threads or on unordered containers.
//
// LINEAGE (NamingOutput::lineage, for the document; the resolver does not need it): for every named input element,
// Split (to the names of its pieces) when it was Modified into several results, Merged (to all names of the
// result) when its result was also the result of other inputs, Deleted when it has no Modified result.
#pragma once
#include "naming_internal.h"

#include <optional>

namespace ray::naming::detail {

// Role given to the pieces of a split element.
inline constexpr std::string_view kSplitRole = "split";

// The hashed parent standing for a sketch curve tag (side faces) or a profile loop tag (caps).
Hash256 curveTagParent(std::uint64_t curveTag);
Hash256 loopTagParent(std::uint64_t loopTag);

// Resolver stages 2 to 4 (resolver.h), run after the Exact stage found no exact answer:
//  2. names followed through this build: a name held by an element that also holds names the reference does not
//     have is Merged; names found on several elements, or found only through "split" records that list the name
//     as a parent, are Split (every piece). Broken instead when the pieces found are fewer than were issued (a
//     piece was split again or removed, and the records of its own pieces are not kept), when a piece also came
//     from names the reference does not stand for (it may hold none of what was picked), when a name is held
//     by several elements, or when only some of a merged element's names are found on one element;
//  3. the family rule: a lost name that was issued alone, whose family now has all its members and more than one,
//     is Split (every member); a lost sibling whose family has exactly one member again (pieces re-joined), or a
//     lost piece whose split parents all lie whole on one element again, is Merged, but only after a geometric
//     check that the element can contain the picked one (same carrier, at least as large, the picked element's
//     position inside it) and that it meets the faces the picked one met (an edge: exactly those; a face: every
//     one of them that still exists);
//  4. Repaired: exactly one element matches the hint, and the faces around it are exactly the faces the
//     reference's neighbour names resolve to.
// Every Split (stage 2 or 3) must also pass a geometric check of its pieces against the hint: together no larger
// than the picked element, and each on the picked element's carrier near where it was, sharing a stretch of a picked
// line or arc (or touching such a piece on the same carrier), or all on one carrier within the picked element's
// length of each other (it moved, then came apart); else Broken. A separate element on the same faces (a cross hole's
// exit loop, a hole's other breakout line, a piece of the face that a smaller cut brings back) is not a piece. A face
// piece must also not meet a face, not around the picked element, of a feature that had a face around it (it would
// lie beyond a boundary the picked element had: facePiecesStayInside). A Merged by the family or split-parent rule
// onto a planar face needs the picked centroid inside the face's outline, not only its box; onto a cylindrical face,
// the picked centroid moved out onto the cylinder, or a point of the picked territory, on the face.
// Every edge or vertex answer must also pass onPickedFaces. nullopt means none applies (the caller reports
// Broken). A name the reference holds that is ambiguous is never followed past the Exact stage (its element
// could have been a sibling). Never throws.
std::optional<Resolved> resolveThroughBuild(const ElementMap& map, const kernel::Shape& shape, const TopoRef& ref);

// An edge is where its faces meet, a vertex where its faces meet: an edge or vertex answer is only trusted while
// every face the reference recorded around the picked element (TopoRef::neighbourNames, followed through this
// build) is still among the faces around the answer. Faces are not checked (a face is its surface). Never throws.
bool onPickedFaces(const ElementMap& map, const kernel::Shape& shape, kernel::SubId element, const TopoRef& ref);

// Does planar face `face` hold, inside its outline, the centroid the reference's hint recorded (the picked element
// kept the place it was picked at)? False for any other surface, or when the outline cannot be read (an edge that is
// not a line, a circle or an open curve). Used for a face among siblings whose geometry changed (resolver.h). Never
// throws.
bool holdsHintCentre(const kernel::Shape& shape, kernel::SubId face, const Fingerprint& hint);

// Does edge `edge`, named from its faces (rule 3) before one of those faces was split, run on past one of its vertices
// as an edge between a piece of that face and the other face (or a piece of it), under another name? Then the edge's
// name stayed on one stretch of the logical edge and the next stretch is named from the piece (T-081a holdout2 1:7:
// a resized hole's wall was split, and the edge where it meets a cut's floor is in two named stretches). Never throws.
bool edgeContinuesUnderOtherName(const ElementMap& map, const kernel::Shape& shape, kernel::SubId edge);

// Is `element` an edge or face of zero measure (an edge no longer than kZeroWidth, a face no wider: its area, read from
// the kernel now, over its box diagonal) while the picked element (`hint`, the reference's) had measure? An element
// whose geometry cannot be read is not. Such an element is never part of an answer: resolveThroughBuild
// leaves zero-measure pieces out of a Split before checking them (and a Split none of whose pieces has any measure is
// Broken), and the resolver leaves them out of every answer. (A degenerate edge picked as such, at a sphere's pole,
// had no measure at pick time either, and still resolves to itself.) Never throws.
bool zeroMeasure(const kernel::Shape& shape, kernel::SubId element, const Fingerprint& hint);

} // namespace ray::naming::detail
