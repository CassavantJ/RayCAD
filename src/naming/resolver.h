// Private to src/naming and its tests (task T-007): the resolver over the bodies of one build.
//
// naming.h declares IResolver but no way to make one; the document rebuilder (T-010) needs this factory in a
// public header. Until then it lives here.
#pragma once
#include <ray/naming.h>

#include <memory>
#include <vector>

namespace ray::naming {

// One body of the current build: its element map and the shape the map was made for.
struct ResolverBody {
    BodyId id;
    ElementMap map;
    kernel::Shape shape;
};

// A resolver over `bodies`. If two bodies share an id, the first one listed is used.
//
// resolve() (PLAN.md 3.3), in stages; T-007 implements the first and the last:
//  1. Exact: every name of the reference is found, all on the same element of the right kind; for any of them
//     that is ambiguous, the element's fingerprint must still match the reference's hint, else Broken.
//  2. (T-081) lineage of the current build: Split / Merged.
//  3. (T-081) family rule: Split / Merged.
//  4. (T-081) Repaired: one fingerprint match surrounded by exactly the faces the neighbour names resolve to.
//  5. Broken, with the reason in words.
// Nothing here throws: an internal failure during resolve() is Broken, and makeResolver returns null only
// when memory runs out.
std::unique_ptr<IResolver> makeResolver(std::vector<ResolverBody> bodies);

} // namespace ray::naming
