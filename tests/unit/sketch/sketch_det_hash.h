// The determinism fingerprint of the sketch solver: a full solve of a noisy S7 grid, then a drag session from the
// solved state (S11 case D1) and the same drag with PlaneGCS's own dense drag, every coordinate of every result hashed
// as raw bytes.
#pragma once

#include "sketch_test_util.h"

#include "solver_internal.h"

#include <ray/sketch.h>

namespace ray::sketch::test {

inline std::uint64_t detHash(const Grid& g, int dragSteps)
{
    auto solver = makePlaneGcsSolver();
    JobContext ctx;
    auto solved = solver->solve(g.sketch, ctx);
    if (!solved) {
        return 0;
    }
    std::uint64_t h = fnv(kFnvBasis, coordinates(solved->solved));
    const SketchEntId pt = g.units.back().bottomP2;
    auto session = solver->beginDrag(solved->solved, pt, ctx);
    if (!session) {
        return 1;
    }
    Vec2 p0{};
    for (const Entity& e : solved->solved.entities) {
        if (e.id == pt) {
            p0 = std::get<Point>(e.geom).p;
        }
    }
    for (int k = 1; k <= dragSteps; ++k) {
        auto r = (*session)->move({p0.x + 0.1 * k, p0.y + 0.05 * k});
        if (!r) {
            return 2;
        }
        h = fnv(h, coordinates(r->solved));
    }
    // The dense path too: PlaneGCS's own drag (the partially redundant fallback), whose QP uses Eigen's blocked
    // matrix products, the part spike S7 found to depend on the detected cache sizes.
    detail::DragOptions planeGcs;
    planeGcs.force = detail::DragMode::PlaneGcs;
    auto dense = detail::beginDragSession(solved->solved, pt, ctx, planeGcs);
    if (!dense) {
        return 3;
    }
    for (int k = 1; k <= dragSteps; ++k) {
        auto r = (*dense)->move({p0.x + 0.1 * k, p0.y + 0.05 * k});
        if (!r) {
            return 4;
        }
        h = fnv(h, coordinates(r->solved));
    }
    return h;
}

} // namespace ray::sketch::test
