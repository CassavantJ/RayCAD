// Private to src/naming and its tests (task T-007): the resolver over the bodies of one build.
//
// ResolverBody and makeResolver are declared in the public naming.h (moved there by T-010 for the document
// rebuilder). The stages resolve() runs:
//  1. Exact: every name of the reference is found, all on the same element of the right kind, and that element
//     holds no other name; for any of them that needs its hint (detail::needsHint: an ordinal, a discriminator made
//     from all faces around, an edge or vertex among siblings), the element's fingerprint must still match the
//     reference's hint, and an edge or vertex must still lie on the faces it was picked on
//     (detail::onPickedFaces), else Broken. Further (T-081a gate): a stub of under 1e-3 mm left by a cut is Broken; a
//     face or edge among siblings that grew is Broken; a face among siblings whose geometry changed must still hold
//     its picked centroid (detail::holdsHintCentre), else Broken; an edge named from a face split since that runs on
//     under another name (detail::edgeContinuesUnderOtherName) is Broken. Then the pick-time evidence
//     (detail::exactAgainstEvidence, territory.h), which includes the completeness check
//     (detail::completeAgainstEvidence: Split with the fuller set, or Broken, when part of the pick is now outside the
//     answer).
//  2. (T-081a) names followed through the current build: Split / Merged.
//  3. (T-081a) family rule and split parent: Split / Merged.
//  4. (T-081a) Repaired: one fingerprint match surrounded by exactly the faces the neighbour names resolve to.
//  5. Broken, with the reason in words.
// Stages 2 to 4 are detail::resolveThroughBuild (propagate.h). A Split or Merged answer from them is checked against
// the evidence too: a Split of one piece as an Exact answer is (exactAgainstEvidence; it stays Split when nothing
// objects, the picked name being gone), any other by the completeness check. No answer to a pick that had measure holds
// an element of zero measure (detail::zeroMeasure): such elements are left out, and an answer left with none is Broken.
#pragma once
#include <ray/naming.h>
