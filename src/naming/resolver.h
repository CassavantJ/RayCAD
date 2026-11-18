// Private to src/naming and its tests (task T-007): the resolver over the bodies of one build.
//
// ResolverBody and makeResolver are declared in the public naming.h (moved there by T-010 for the document
// rebuilder). The stages resolve() runs:
//  1. Exact: every name of the reference is found, all on the same element of the right kind; for any of them
//     that is ambiguous, the element's fingerprint must still match the reference's hint, else Broken.
//  2. (T-081) lineage of the current build: Split / Merged.
//  3. (T-081) family rule: Split / Merged.
//  4. (T-081) Repaired: one fingerprint match surrounded by exactly the faces the neighbour names resolve to.
//  5. Broken, with the reason in words.
#pragma once
#include <ray/naming.h>
