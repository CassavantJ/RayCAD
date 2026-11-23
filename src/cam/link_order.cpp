// The order of the passes (link.h orderPasses, orient, travelCost, validOrder; task T-051).
//
// THE COST of going from one pass to the next is the rapid length that link takes when it retracts: up from the end
// of the cut to the retract plane (from the feed plane unless rapidRetract), across, and down to the feed plane; a link
// that can stay down (within Options::maxStayDown, the same Z, the same cluster) costs its XY length instead. A pass
// is entered at its lead-in's start at the lead's nominal size and left at its lead-out's end; a rotatable pass at the
// point of its path nearest to where the tool is (its start then, being closed, is also its end).
//
// HOW, inside each sequence batch (batches in increasing sequence; a cluster's passes in increasing level, so a pass
// is ready once every pass of its cluster with a lower level is done):
//   1. Nearest neighbour: from where the tool is, the ready pass and orientation with the cheapest link (ties: the
//      lower pass index, then forward). With no start point (Options::from): when the first batch has at most
//      kImprovedStarts passes, the whole tour is built and improved (step 2) from every ready pass and orientation as
//      the first, and the cheapest kept; up to kMultiStartMax, the first pass is the one whose nearest-neighbour tour
//      of the first batch is cheapest.
//   2. Improvement, once every batch is placed (so a batch's last link, into the next batch, counts), at most kRounds
//      rounds over all batches while something improves: Or-opt (a run of 1 to 3 passes moved, as it is or
//      reversed, to a place at most kWindow positions away) and 2-opt (a run of at most kWindow + 1 passes reversed;
//      reversible passes flip their direction). A change is kept when it respects the levels and lowers the cost of
//      the links it touches by more than 1 nm (the rotatable passes after it are re-placed and counted, up to
//      kRotatableChain of them) and lowers the whole tour's cost.
//   3. The direction of every reversible pass and the start of every rotatable one by dynamic programming over the
//      order (orient): at most two states per position (forward, reversed), a rotatable pass placed from each state.
// Cost: nearest neighbour O(n^2), each improvement round about 7 n kWindow link evaluations of O(kWindow) each, the
// orientation O(n). Deterministic: integer coordinates, double costs from integer differences, ties by index.
#include "link_internal.h"

#include <algorithm>
#include <limits>
#include <map>

namespace ray::cam::link {

using namespace detail;

namespace {

constexpr std::size_t kWindow = 24;     // batches of at most kSmallBatch passes
constexpr std::size_t kWindowLarge = 6; // batches of at most kLargeBatch passes; larger ones are not improved
constexpr std::size_t kSmallBatch = 400;
constexpr std::size_t kLargeBatch = 4000;
constexpr int kRounds = 8;
constexpr std::size_t kMultiStartMax = 64;  // first batches up to this size: the best nearest-neighbour start
constexpr std::size_t kImprovedStarts = 12; // up to this size: every start built and improved
constexpr std::size_t kRotatableChain = 32;
constexpr double kInf = std::numeric_limits<double>::infinity();

struct Prep {
    const Pass* pass = nullptr;
    int options = 1; // 2 when reversible
    bool rotatable = false;
    Ends ends[2];                // per orientation (for a rotatable pass: its start as given)
    std::optional<Path> ring[2]; // rotatable: the oriented closed path
};

struct Node {
    std::size_t pass = 0;
    int o = 0;
    bool operator==(const Node&) const = default;
};

// Where the tool is after a pass (any = false: before the first).
struct Where {
    Pt3 e;
    std::size_t pass = 0;
    bool any = false;
};

// The arc length along `p` of its point nearest to q (the first such point).
double nearestS(const Path& p, Pt q)
{
    double best = kInf, bestS = 0, at = 0;
    Pt cur = p.start;
    const D2 qd = d2(q);
    for (const geom2d::Span& s : p.spans) {
        if (s.kind == geom2d::SpanKind::Line) {
            const D2 a = d2(cur), b = d2(s.end), ab = b - a;
            const double l2 = ab.x * ab.x + ab.y * ab.y;
            const double len = dmath::sqrt(l2);
            double t = l2 > 0 ? ((qd.x - a.x) * ab.x + (qd.y - a.y) * ab.y) / l2 : 0.0;
            t = std::clamp(t, 0.0, 1.0);
            const double d = norm(a + ab * t - qd);
            if (d < best) {
                best = d;
                bestS = at + t * len;
            }
            at += len;
        } else {
            const bool ccw = s.kind == geom2d::SpanKind::ArcCCW;
            const D2 c = d2(s.centre);
            const double r = static_cast<double>(s.radius);
            const double sweep = geom2d::sweepOf(cur, s);
            const double a0 =
                dmath::atan2(static_cast<double>(cur.y - s.centre.y), static_cast<double>(cur.x - s.centre.x));
            const D2 rq = qd - c;
            const double phi = dmath::atan2(rq.y, rq.x);
            double delta = ccw ? phi - a0 : a0 - phi;
            while (delta < 0)
                delta += 2 * kPi;
            while (delta >= 2 * kPi)
                delta -= 2 * kPi;
            const double dStart = norm(d2(cur) - qd);
            if (dStart < best) {
                best = dStart;
                bestS = at;
            }
            if (delta <= sweep) {
                const double d = std::abs(norm(rq) - r);
                if (d < best) {
                    best = d;
                    bestS = at + r * delta;
                }
            }
            const double dEnd = norm(d2(s.end) - qd);
            if (dEnd < best) {
                best = dEnd;
                bestS = at + r * sweep;
            }
            at += r * sweep;
        }
        cur = s.end;
    }
    return bestS >= at ? 0.0 : bestS; // a closed path's end is its start
}

class Orderer {
public:
    Orderer(std::span<const Pass> passes, const Options& opts) : passes_(passes), opts_(opts)
    {
        prep_.resize(passes.size());
        for (std::size_t i = 0; i < passes.size(); ++i) {
            Prep& p = prep_[i];
            p.pass = &passes[i];
            p.options = passes[i].reversible ? 2 : 1;
            p.rotatable = passes[i].rotatable;
            for (int o = 0; o < p.options; ++o) {
                const Shape s = placedShape(passes[i], Placement{i, o == 1, 0});
                p.ends[o] = nominalEnds(passes[i], s);
                if (p.rotatable && s.path)
                    p.ring[o] = s.path;
            }
            if (p.rotatable && !p.ring[0])
                p.rotatable = false;
        }
    }

    double link(const Where& w, std::size_t to, const Pt3& a) const
    {
        if (!w.any)
            return opts_.from ? distXY(*opts_.from, xyOf(a)) : 0.0;
        const double d = distXY(xyOf(w.e), xyOf(a));
        if (opts_.maxStayDown > 0 && d <= static_cast<double>(opts_.maxStayDown) && w.e.z == a.z &&
            passes_[w.pass].cluster == passes_[to].cluster)
            return d;
        const double up = static_cast<double>(opts_.retract - (opts_.rapidRetract ? w.e.z : opts_.feedPlane));
        const double down = static_cast<double>(opts_.retract - opts_.feedPlane);
        return up + d + down;
    }

    // The ends of `n` entered from w; s: the start of a rotatable pass.
    Ends place(const Node& n, const Where& w, double* s = nullptr) const
    {
        const Prep& p = prep_[n.pass];
        if (!p.rotatable || !w.any) {
            if (s != nullptr)
                *s = 0;
            return p.ends[n.o];
        }
        const Path& ring = *p.ring[n.o];
        const double at = nearestS(ring, xyOf(w.e));
        if (s != nullptr)
            *s = at;
        const Pt q = geom2d::pointAt(ring, at);
        const Pt3 q3{q.x, q.y, p.pass->start.z};
        return Ends{q3, q3};
    }

    double step(Where& w, const Node& n) const
    {
        const Ends e = place(n, w);
        const double c = link(w, n.pass, e.a);
        w = Where{e.e, n.pass, true};
        return c;
    }

    int flip(const Node& n) const { return prep_[n.pass].options == 2 ? 1 - n.o : n.o; }

    // ---------- nearest neighbour ----------
    struct Pending {
        std::map<int, std::map<int, int>> levels; // cluster -> level -> count
        void add(const Pass& p)
        {
            if (p.cluster >= 0)
                ++levels[p.cluster][p.level];
        }
        void remove(const Pass& p)
        {
            if (p.cluster < 0)
                return;
            auto& l = levels[p.cluster];
            if (--l[p.level] == 0)
                l.erase(p.level);
        }
        bool ready(const Pass& p) const
        {
            if (p.cluster < 0)
                return true;
            const auto it = levels.find(p.cluster);
            return it == levels.end() || it->second.empty() || it->second.begin()->first >= p.level;
        }
    };

    Result<double> nearest(const std::vector<std::size_t>& batch, Where w, std::optional<Node> first,
                           std::vector<Node>& out, JobContext& ctx) const
    {
        Pending pending;
        for (const std::size_t i : batch)
            pending.add(passes_[i]);
        std::vector<bool> done(batch.size(), false);
        double total = 0;
        for (std::size_t count = 0; count < batch.size(); ++count) {
            if (count % 64 == 0)
                if (auto st = ctx.cancel.check(); !st)
                    return tl::unexpected(st.error());
            double best = kInf;
            std::size_t bestK = batch.size();
            Node bestNode;
            if (count == 0 && first) {
                bestNode = *first;
                bestK = static_cast<std::size_t>(std::find(batch.begin(), batch.end(), first->pass) - batch.begin());
            } else {
                for (std::size_t k = 0; k < batch.size(); ++k) {
                    if (done[k] || !pending.ready(passes_[batch[k]]))
                        continue;
                    for (int o = 0; o < prep_[batch[k]].options; ++o) {
                        const Node n{batch[k], o};
                        const double c = link(w, n.pass, place(n, w).a);
                        if (c < best) {
                            best = c;
                            bestK = k;
                            bestNode = n;
                        }
                    }
                }
            }
            if (bestK >= batch.size())
                return tl::unexpected(failure("The order of the passes cannot respect their levels."));
            done[bestK] = true;
            pending.remove(passes_[batch[bestK]]);
            total += step(w, bestNode);
            out.push_back(bestNode);
        }
        return total;
    }

    // ---------- improvement ----------
    void recompute()
    {
        after_.assign(tour_.size(), Where{});
        Where w;
        if (start_)
            w = *start_;
        total_ = 0;
        for (std::size_t k = 0; k < tour_.size(); ++k) {
            total_ += step(w, tour_[k]);
            after_[k] = w;
        }
    }
    Where before(std::size_t k) const { return k == 0 ? Where{} : after_[k - 1]; }

    bool levelsHold(const std::vector<Node>& slice) const
    {
        std::map<int, int> last;
        for (const Node& n : slice) {
            const Pass& p = passes_[n.pass];
            if (p.cluster < 0)
                continue;
            const auto it = last.find(p.cluster);
            if (it != last.end() && p.level < it->second)
                return false;
            last[p.cluster] = p.level;
        }
        return true;
    }

    // The cost of the links into positions lo.. of a tour whose positions [lo, lo + slice.size()) hold `slice`,
    // through the rotatable passes after it.
    double sliceCost(std::size_t lo, const std::vector<Node>& slice) const
    {
        Where w = before(lo);
        double c = 0;
        for (const Node& n : slice)
            c += step(w, n);
        for (std::size_t k = lo + slice.size(), m = 0; k < tour_.size() && m <= kRotatableChain; ++k, ++m) {
            c += step(w, tour_[k]);
            if (!prep_[tour_[k].pass].rotatable)
                break;
        }
        return c;
    }

    bool tryReplace(std::size_t lo, std::vector<Node> slice)
    {
        if (!levelsHold(slice))
            return false;
        const std::vector<Node> old(tour_.begin() + static_cast<std::ptrdiff_t>(lo),
                                    tour_.begin() + static_cast<std::ptrdiff_t>(lo + slice.size()));
        if (old == slice)
            return false;
        const double before = sliceCost(lo, old);
        const double after = sliceCost(lo, slice);
        if (!(after < before - 1.0))
            return false;
        const double oldTotal = total_;
        std::copy(slice.begin(), slice.end(), tour_.begin() + static_cast<std::ptrdiff_t>(lo));
        recompute();
        if (total_ < oldTotal - 1.0)
            return true;
        std::copy(old.begin(), old.end(), tour_.begin() + static_cast<std::ptrdiff_t>(lo));
        recompute();
        return false;
    }

    Result<bool> improveRound(std::size_t b0, std::size_t b1, std::size_t window, JobContext& ctx)
    {
        bool improved = false;
        // Or-opt.
        for (std::size_t len = 1; len <= 3; ++len) {
            for (std::size_t i = b0; i + len <= b1; ++i) {
                if (auto st = ctx.cancel.check(); !st)
                    return tl::unexpected(st.error());
                const std::size_t jLo = i > b0 + window ? i - window : b0;
                const std::size_t jHi = std::min(b1, i + len + window);
                for (std::size_t j = jLo; j <= jHi; ++j) {
                    if (j >= i && j <= i + len)
                        continue;
                    for (const bool rev : {false, true}) {
                        std::vector<Node> seg(tour_.begin() + static_cast<std::ptrdiff_t>(i),
                                              tour_.begin() + static_cast<std::ptrdiff_t>(i + len));
                        if (rev) {
                            std::reverse(seg.begin(), seg.end());
                            for (Node& n : seg)
                                n.o = flip(n);
                        }
                        std::vector<Node> slice;
                        std::size_t lo = 0;
                        if (j < i) {
                            lo = j;
                            slice = seg;
                            slice.insert(slice.end(), tour_.begin() + static_cast<std::ptrdiff_t>(j),
                                         tour_.begin() + static_cast<std::ptrdiff_t>(i));
                        } else {
                            lo = i;
                            slice.assign(tour_.begin() + static_cast<std::ptrdiff_t>(i + len),
                                         tour_.begin() + static_cast<std::ptrdiff_t>(j));
                            slice.insert(slice.end(), seg.begin(), seg.end());
                        }
                        if (tryReplace(lo, std::move(slice)))
                            improved = true;
                    }
                }
            }
        }
        // 2-opt.
        for (std::size_t i = b0; i + 1 < b1; ++i) {
            if (auto st = ctx.cancel.check(); !st)
                return tl::unexpected(st.error());
            for (std::size_t j = i + 1; j < b1 && j <= i + window; ++j) {
                std::vector<Node> slice(tour_.begin() + static_cast<std::ptrdiff_t>(i),
                                        tour_.begin() + static_cast<std::ptrdiff_t>(j + 1));
                std::reverse(slice.begin(), slice.end());
                for (Node& n : slice)
                    n.o = flip(n);
                if (tryReplace(i, std::move(slice)))
                    improved = true;
            }
        }
        return improved;
    }

    // The batches in increasing sequence: each a list of pass indices.
    std::vector<std::vector<std::size_t>> batches() const
    {
        std::vector<std::size_t> idx(passes_.size());
        for (std::size_t i = 0; i < idx.size(); ++i)
            idx[i] = i;
        std::stable_sort(idx.begin(), idx.end(),
                         [&](std::size_t a, std::size_t b) { return passes_[a].sequence < passes_[b].sequence; });
        std::vector<std::vector<std::size_t>> out;
        for (std::size_t b = 0; b < idx.size();) {
            std::size_t e = b;
            while (e < idx.size() && passes_[idx[e]].sequence == passes_[idx[b]].sequence)
                ++e;
            out.emplace_back(idx.begin() + static_cast<std::ptrdiff_t>(b),
                             idx.begin() + static_cast<std::ptrdiff_t>(e));
            b = e;
        }
        return out;
    }

    // A whole tour: nearest neighbour batch by batch (the first from `first` when given; else, when `pickStart`, from
    // the start whose nearest-neighbour tour of the first batch is cheapest), then improved. Its cost is total_.
    Status build(const std::vector<std::vector<std::size_t>>& groups, std::optional<Node> first, bool pickStart,
                 JobContext& ctx)
    {
        tour_.clear();
        Where w;
        std::vector<std::pair<std::size_t, std::size_t>> ranges; // each batch's positions in the tour
        for (std::size_t g = 0; g < groups.size(); ++g) {
            const std::vector<std::size_t>& batch = groups[g];
            const std::size_t b0 = tour_.size();
            std::optional<Node> start = g == 0 ? first : std::nullopt;
            if (g == 0 && !start && pickStart) {
                Pending pending;
                for (const std::size_t i : batch)
                    pending.add(passes_[i]);
                double best = kInf;
                for (const std::size_t i : batch) {
                    if (!pending.ready(passes_[i]))
                        continue;
                    for (int o = 0; o < prep_[i].options; ++o) {
                        std::vector<Node> t;
                        auto c = nearest(batch, w, Node{i, o}, t, ctx);
                        if (!c)
                            return tl::unexpected(c.error());
                        if (*c < best) {
                            best = *c;
                            start = Node{i, o};
                        }
                    }
                }
            }
            std::vector<Node> t;
            auto c = nearest(batch, w, start, t, ctx);
            if (!c)
                return tl::unexpected(c.error());
            tour_.insert(tour_.end(), t.begin(), t.end());
            recompute();
            ranges.emplace_back(b0, tour_.size());
            w = after_.back();
        }
        // Improve every batch with the whole tour in place, so the links into the next batch count too.
        for (int round = 0; round < kRounds; ++round) {
            bool improved = false;
            for (const auto& [b0, b1] : ranges) {
                const std::size_t size = b1 - b0;
                const std::size_t window = size <= kSmallBatch ? kWindow : (size <= kLargeBatch ? kWindowLarge : 0);
                if (window == 0 || size < 2)
                    continue;
                auto better = improveRound(b0, b1, window, ctx);
                if (!better)
                    return tl::unexpected(better.error());
                improved = improved || *better;
            }
            if (!improved)
                break;
        }
        return {};
    }

    Result<std::vector<std::size_t>> run(JobContext& ctx)
    {
        const std::vector<std::vector<std::size_t>> groups = batches();
        const bool open = !opts_.from && !groups.empty() && groups[0].size() > 1;
        if (open && groups[0].size() <= kImprovedStarts) {
            // Small: every start built and improved, the cheapest kept (ties: the first tried).
            Pending pending;
            for (const std::size_t i : groups[0])
                pending.add(passes_[i]);
            double best = kInf;
            std::vector<Node> bestTour;
            for (const std::size_t i : groups[0]) {
                if (!pending.ready(passes_[i]))
                    continue;
                for (int o = 0; o < prep_[i].options; ++o) {
                    if (auto st = build(groups, Node{i, o}, false, ctx); !st)
                        return tl::unexpected(st.error());
                    if (total_ < best - 1.0) {
                        best = total_;
                        bestTour = tour_;
                    }
                }
            }
            tour_ = std::move(bestTour);
            recompute();
        } else if (auto st = build(groups, std::nullopt, open && groups[0].size() <= kMultiStartMax, ctx); !st) {
            return tl::unexpected(st.error());
        }
        std::vector<std::size_t> order;
        order.reserve(tour_.size());
        for (const Node& n : tour_)
            order.push_back(n.pass);
        return order;
    }
    // ---------- orientation ----------
    std::vector<Placement> orientation(std::span<const std::size_t> order) const
    {
        struct State {
            double cost = 0;
            int back = -1;
            int o = 0;
            double s = 0;
            Where w;
        };
        std::vector<std::vector<State>> dp(order.size());
        for (std::size_t k = 0; k < order.size(); ++k) {
            const Prep& p = prep_[order[k]];
            for (int o = 0; o < p.options; ++o) {
                State best;
                best.cost = kInf;
                const std::size_t prevCount = k == 0 ? 1 : dp[k - 1].size();
                for (std::size_t q = 0; q < prevCount; ++q) {
                    const Where w = k == 0 ? Where{} : dp[k - 1][q].w;
                    const double base = k == 0 ? 0.0 : dp[k - 1][q].cost;
                    double s = 0;
                    const Ends e = place(Node{order[k], o}, w, &s);
                    const double c = base + link(w, order[k], e.a);
                    if (c < best.cost) {
                        best = State{c, static_cast<int>(q), o, s, Where{e.e, order[k], true}};
                    }
                }
                dp[k].push_back(best);
            }
        }
        std::vector<Placement> out(order.size());
        if (order.empty())
            return out;
        int q = 0;
        for (int i = 1; i < static_cast<int>(dp.back().size()); ++i)
            if (dp.back()[static_cast<std::size_t>(i)].cost < dp.back()[static_cast<std::size_t>(q)].cost)
                q = i;
        for (std::size_t k = order.size(); k-- > 0;) {
            const State& st = dp[k][static_cast<std::size_t>(q)];
            out[k] = Placement{order[k], st.o == 1, st.s};
            q = st.back;
        }
        return out;
    }

private:
    std::span<const Pass> passes_;
    const Options& opts_;
    std::vector<Prep> prep_;
    std::vector<Node> tour_;
    std::vector<Where> after_;
    std::optional<Where> start_;
    double total_ = 0;
};

} // namespace

bool validOrder(std::span<const Pass> passes, std::span<const std::size_t> order)
{
    std::map<int, int> last;
    int sequence = std::numeric_limits<int>::min();
    std::vector<bool> seen(passes.size(), false);
    if (order.size() != passes.size())
        return false;
    for (const std::size_t i : order) {
        if (i >= passes.size() || seen[i])
            return false;
        seen[i] = true;
        const Pass& p = passes[i];
        if (p.sequence < sequence)
            return false;
        if (p.sequence != sequence)
            last.clear(); // levels order the passes of a cluster inside one sequence batch
        sequence = p.sequence;
        if (p.cluster >= 0) {
            const auto it = last.find(p.cluster);
            if (it != last.end() && p.level < it->second)
                return false;
            last[p.cluster] = p.level;
        }
    }
    return true;
}

std::vector<Placement> orient(std::span<const Pass> passes, std::span<const std::size_t> order, const Options& opts)
{
    return Orderer(passes, opts).orientation(order);
}

Result<std::vector<Placement>> orderPasses(std::span<const Pass> passes, const Options& opts, JobContext& ctx)
{
    Orderer o(passes, opts);
    auto order = o.run(ctx);
    if (!order)
        return tl::unexpected(order.error());
    return o.orientation(*order);
}

double travelCost(std::span<const Pass> passes, std::span<const Placement> order, const Options& opts)
{
    if (order.empty())
        return 0;
    double cost = static_cast<double>(opts.clearance - opts.feedPlane);
    std::optional<Ends> prev;
    std::size_t prevPass = 0;
    for (const Placement& pl : order) {
        const Pass& pass = passes[pl.pass];
        const Ends e = nominalEnds(pass, placedShape(pass, pl));
        if (!prev) {
            if (opts.from)
                cost += distXY(*opts.from, xyOf(e.a));
        } else {
            const double d = distXY(xyOf(prev->e), xyOf(e.a));
            if (opts.maxStayDown > 0 && d <= static_cast<double>(opts.maxStayDown) && prev->e.z == e.a.z &&
                passes[prevPass].cluster == pass.cluster) {
                cost += d;
            } else {
                cost += static_cast<double>(opts.retract - (opts.rapidRetract ? prev->e.z : opts.feedPlane)) + d +
                        static_cast<double>(opts.retract - opts.feedPlane);
            }
        }
        prev = e;
        prevPass = pl.pass;
    }
    cost += static_cast<double>(opts.clearance - (opts.rapidRetract ? prev->e.z : opts.feedPlane));
    return cost;
}

} // namespace ray::cam::link
