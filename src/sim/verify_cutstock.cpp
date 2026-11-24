// The cut-stock layer of the safety checks (task T-071; verify.h says when it runs and what it reports). The program is
// cut, in order, into a CPU tri-dexel model of the initial stock at the fixed cell cutStockCell() (never a tier's), up
// to the last motion the analytic layer deferred. Batches of kCutStockBatch motions of one operation go through
// cutBatch (removed volume and nonCuttingHits per motion). A batch holding a deferred motion is cut from a snapshot;
// when one of its deferred motions shows something (a rapid or a plunge that removed material, a shank or holder hit),
// the batch is restored and replayed motion by motion, asking IStockModel::intersects at the right point: a rapid
// (whole tool) and a plunge (the dead zone) before the motion cuts, the shank and the holder before it unless it goes
// down, after it when it does (as nonCuttingHits); the motions between those still go through cutBatch together.
// Results do not depend on the thread count or on how the motions are batched (the model's contract), so neither do
// these.
#include "verify_impl.h"

#include <algorithm>

namespace ray::sim::detail {

Result<CutStockResult> runCutStock(const VerifyInput& in, const std::vector<Motion>& motions,
                                   const std::vector<ToolZones>& zones, const std::vector<Deferred>& deferred,
                                   IJobSystem& jobs, JobContext& ctx, Progress progress)
{
    CutStockResult res;
    if (deferred.empty() || in.stock == nullptr || in.stock->positions.size() < 3) {
        return res;
    }
    Box3 bounds{{in.stock->positions[0], in.stock->positions[1], in.stock->positions[2]},
                {in.stock->positions[0], in.stock->positions[1], in.stock->positions[2]}};
    for (std::size_t i = 0; i + 2 < in.stock->positions.size(); i += 3) {
        const Vec3 p{in.stock->positions[i], in.stock->positions[i + 1], in.stock->positions[i + 2]};
        bounds.min = {std::min(bounds.min.x, p.x), std::min(bounds.min.y, p.y), std::min(bounds.min.z, p.z)};
        bounds.max = {std::max(bounds.max.x, p.x), std::max(bounds.max.y, p.y), std::max(bounds.max.z, p.z)};
    }
    res.cellMm = cutStockCell(bounds);
    std::unique_ptr<IStockModel> model = makeTriDexelCpu(jobs);
    model->reset(*in.stock, bounds, StockResolution{res.cellMm});

    const std::uint32_t last = deferred.back().motion;
    std::vector<std::uint8_t> bits(static_cast<std::size_t>(last) + 1, 0);
    for (const Deferred& d : deferred) {
        bits[d.motion] |= d.bits;
    }
    const auto add = [&](std::uint32_t k, EventKind kind, const Vec3& at) {
        RawHit h;
        h.op = motions[k].op;
        h.move = motions[k].move;
        h.motion = k;
        h.kind = kind;
        h.obstacle = kStock;
        h.layer = Layer::CutStock;
        h.position = at;
        h.havePosition = true;
        res.hits.push_back(h);
    };
    // The flute length and the extra stick-out found so far per operation: a later hit that they clear needs no search
    // of its own (a program whose every move hits would otherwise search at every move).
    std::vector<double> shankNeed(zones.size(), -1.0), holderNeed(zones.size(), -1.0);
    // Shank and holder of motion k against the material as it is now.
    const auto nonCutting = [&](std::uint32_t k) {
        const Motion& m = motions[k];
        const ToolZones& z = zones[m.op];
        const Vec3 at = m.b.z < m.a.z ? m.b : m.a;
        if ((bits[k] & kDeferShank) != 0 && !z.shankOnly.shankProfile.empty() &&
            model->intersects(z.shankOnly, m.a, m.b)) {
            add(k, EventKind::ShankHitsStock, at);
            const double span = std::max(0.0, z.shankTop - z.fluteTop);
            const auto hits = [&](double s) {
                const ToolShape above = shankAboveShape(z, z.fluteTop + s);
                return !above.shankProfile.empty() && model->intersects(above, m.a, m.b);
            };
            double& known = shankNeed[m.op];
            if (!(known >= 0 && !hits(known))) {
                known = std::max(known, std::min(smallestClear(span, hits), span));
            }
            res.lengths.push_back({k, false, z.fluteTop + known, true});
        }
        if ((bits[k] & kDeferHolder) != 0 && model->intersects(z.holderOnly, m.a, m.b)) {
            add(k, EventKind::HolderHitsStock, at);
            const auto hits = [&](double lift) {
                return model->intersects(liftedHolderShape(z, lift), m.a, m.b);
            };
            double& known = holderNeed[m.op];
            if (known >= 0 && !hits(known)) {
                res.lengths.push_back({k, true, known, true});
            } else {
                const double s = smallestClear(kLiftCapMm, hits);
                if (s <= kLiftCapMm) {
                    known = std::max(known, s);
                }
                res.lengths.push_back({k, true, s, s <= kLiftCapMm});
            }
        }
    };

    std::vector<IStockModel::CutMove> moves;
    std::vector<double> removed;
    std::vector<std::uint8_t> hits;
    std::uint32_t i = 0;
    while (i <= last) {
        if (Status s = ctx.cancel.check(); !s) {
            return tl::unexpected(s.error());
        }
        progress.set(static_cast<double>(i) / static_cast<double>(last + 1), "Checking against the cut stock");
        const std::uint32_t op = motions[i].op;
        std::uint32_t j = i;
        bool anyDeferred = false;
        moves.clear();
        while (j <= last && motions[j].op == op && moves.size() < kCutStockBatch) {
            moves.push_back({motions[j].a, motions[j].b, motions[j].rapid});
            anyDeferred = anyDeferred || bits[j] != 0;
            ++j;
        }
        const ToolShape& shape = zones[op].full;
        if (!anyDeferred) {
            model->cutBatch(shape, moves, {}, {});
            i = j;
            continue;
        }
        const std::shared_ptr<const IStockModel::Snapshot> snap = model->snapshot();
        removed.assign(moves.size(), 0.0);
        hits.assign(moves.size(), 0);
        model->cutBatch(shape, moves, removed, hits);
        const auto suspicious = [&](std::uint32_t k) {
            const std::size_t e = k - i;
            const std::uint8_t b = bits[k];
            return ((b & kDeferRapid) != 0 && (removed[e] > 0 || hits[e] != 0)) ||
                   ((b & (kDeferShank | kDeferHolder)) != 0 && hits[e] != 0) ||
                   ((b & kDeferPlunge) != 0 && removed[e] > 0);
        };
        bool replay = false;
        for (std::uint32_t k = i; k < j && !replay; ++k) {
            replay = bits[k] != 0 && suspicious(k);
        }
        if (!replay) {
            i = j;
            continue;
        }
        model->restore(*snap);
        // The motions between suspicious ones go through cutBatch together (the same stock as one at a time).
        std::size_t pending = 0; // moves[pending, k - i) wait to be cut
        const auto flush = [&](std::uint32_t k) {
            const std::size_t upto = k - i;
            if (upto > pending) {
                model->cutBatch(shape, std::span<const IStockModel::CutMove>(moves.data() + pending, upto - pending),
                                {}, {});
            }
            pending = upto;
        };
        for (std::uint32_t k = i; k < j; ++k) {
            const Motion& m = motions[k];
            const ToolZones& z = zones[op];
            if (bits[k] == 0 || !suspicious(k)) {
                continue;
            }
            // A motion asked here costs a few stock queries (more when a stick-out or flute length is searched).
            if (Status s = ctx.cancel.check(); !s) {
                return tl::unexpected(s.error());
            }
            flush(k);
            pending = k - i + 1; // this one is cut here
            const bool down = m.b.z < m.a.z;
            if ((bits[k] & kDeferRapid) != 0 && model->intersects(z.full, m.a, m.b)) {
                add(k, EventKind::RapidIntoCutStock, m.a);
            }
            if ((bits[k] & kDeferPlunge) != 0 && !z.deadOnly.cuttingProfile.empty() &&
                model->intersects(z.deadOnly, m.a, m.b)) {
                add(k, EventKind::PlungeWithNonCentreCutting, m.a);
            }
            if (!down) {
                nonCutting(k);
            }
            model->cut(shape, m.a, m.b, m.rapid);
            if (down) {
                nonCutting(k);
            }
        }
        flush(j);
        i = j;
    }
    progress.set(1.0, "Checking against the cut stock");
    return res;
}

} // namespace ray::sim::detail
