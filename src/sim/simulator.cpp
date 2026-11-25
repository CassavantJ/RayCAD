// The simulator (task T-072): makeSimulator. How it works and what it promises: simulator.h and include/ray/sim.h.
#include "simulator.h"

#include "tridexel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <typeinfo>

namespace ray::sim {

namespace detail {

namespace {

constexpr double kPi = 3.14159265358979323846;

// One operation of the last run.
struct OpTable {
    OperationId id;
    std::uint32_t moves = 0;       // IR moves (0 without a toolpath)
    std::uint64_t firstMotion = 0; // its sub-motions are [firstMotion, endMotion)
    std::uint64_t endMotion = 0;
    std::vector<std::uint64_t> moveFirst; // moves + 1 entries: the sub-motions of move m are [moveFirst[m], [m + 1])
    Vec3 start{};                         // the toolpath's start point
    bool hasPath = false;
    double radius = 0, height = 0; // of the cutting profile, for rayCost
};

struct Keyframe {
    std::uint64_t motion = 0; // sub-motions cut
    std::shared_ptr<const IStockModel::Snapshot> snap;
};

Box3 soupBox(const kernel::TriSoup& s)
{
    Box3 b{{0, 0, 0}, {-1, -1, -1}};
    for (std::size_t i = 0; i + 2 < s.positions.size(); i += 3) {
        const Vec3 p{s.positions[i], s.positions[i + 1], s.positions[i + 2]};
        if (i == 0) {
            b = {p, p};
            continue;
        }
        b.min = {std::min(b.min.x, p.x), std::min(b.min.y, p.y), std::min(b.min.z, p.z)};
        b.max = {std::max(b.max.x, p.x), std::max(b.max.y, p.y), std::max(b.max.z, p.z)};
    }
    return b;
}

// The cell the stock model snaps to (tridexel.cpp reset; without its memory cap, which only coarsens).
Vec3 snappedCell(const Box3& b, double cell)
{
    const double c = cell > 0 && std::isfinite(cell) ? cell : 0.2;
    const auto snap = [c](double ext) {
        return ext / std::max(1.0, std::ceil(ext / c - 1e-9));
    };
    return {snap(b.max.x - b.min.x), snap(b.max.y - b.min.y), snap(b.max.z - b.min.z)};
}

double maxRadius(const std::vector<Vec2>& p)
{
    double r = 0;
    for (const Vec2& v : p) {
        r = std::max(r, v.x);
    }
    return r;
}

Vec3 lerp(const Vec3& a, const Vec3& b, double t)
{
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

double length(const Motion& m)
{
    const double dx = m.b.x - m.a.x, dy = m.b.y - m.a.y, dz = m.b.z - m.a.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

Error error(ErrorCode code, std::string message)
{
    return Error{code, std::move(message), {"sim.simulator"}};
}

} // namespace

double rayCost(const Motion& m, double radius, double height, const Box3& stock, const Vec3& cell)
{
    const double x0 = std::max(std::min(m.a.x, m.b.x) - radius, stock.min.x);
    const double x1 = std::min(std::max(m.a.x, m.b.x) + radius, stock.max.x);
    const double y0 = std::max(std::min(m.a.y, m.b.y) - radius, stock.min.y);
    const double y1 = std::min(std::max(m.a.y, m.b.y) + radius, stock.max.y);
    const double z0 = std::max(std::min(m.a.z, m.b.z), stock.min.z);
    const double z1 = std::min(std::max(m.a.z, m.b.z) + height, stock.max.z);
    if (!(x1 > x0 && y1 > y0 && z1 > z0) || !(cell.x > 0 && cell.y > 0 && cell.z > 0)) {
        return kMotionRays;
    }
    const double dx = m.b.x - m.a.x, dy = m.b.y - m.a.y;
    const double sweptArea = 2.0 * radius * std::sqrt(dx * dx + dy * dy) + kPi * radius * radius;
    const double zArea = std::min((x1 - x0) * (y1 - y0), sweptArea);
    return kMotionRays + zArea / (cell.x * cell.y) + (y1 - y0) * (z1 - z0) / (cell.y * cell.z) +
           (x1 - x0) * (z1 - z0) / (cell.x * cell.z);
}

class Simulator final : public ISimulator {
public:
    Simulator(IJobSystem& jobs, std::unique_ptr<IStockModel> stock) : jobs_(jobs), stock_(std::move(stock))
    {
        if (!stock_) {
            stock_ = makeTriDexelCpu(jobs_);
        }
        // The CPU model's class is private to tridexel.cpp: compare with a fresh one's dynamic type. Only it can count
        // the bytes a keyframe pins (snapshotPrivateBytes).
        const std::unique_ptr<IStockModel> probe = makeTriDexelCpu(jobs_);
        const IStockModel& mine = *stock_;
        const IStockModel& cpu = *probe;
        cpu_ = typeid(mine) == typeid(cpu);
    }

    Result<std::vector<SimEvent>> verifyFast(const SimProgram& p, JobContext& ctx) override
    {
        return verifyProgram(verifyInputFrom(p), jobs_, ctx);
    }
    Status run(const SimProgram& p, const SimOptions& opts, JobContext& ctx) override;
    std::span<const SimEvent> events() const override { return events_; }
    Status seek(TimelinePosition pos) override;
    const IStockModel& stock() const override { return *stock_; }
    Xform toolPose() const override;
    std::uint64_t totalMoves() const override { return totalMoves_; }
    kernel::TriSoup finalStock() const override;
    TimelinePosition position() const override { return pos_; }
    TimelinePosition runEnd() const override { return endPos_; }
    SeekReport seekReport() const override { return report_; }

    void setTuning(const SimTuning& t) { tuning_ = t; }
    SimRunInfo info() const
    {
        SimRunInfo in = info_;
        in.ready = ready_;
        for (const Keyframe& k : keyframes_) {
            in.keyframeMotions.push_back(k.motion);
        }
        in.keyframeBytes = cpu_ ? ledger_.pinnedBytes() : 0;
        return in;
    }

private:
    // A position resolved: `full` sub-motions cut whole, then the fraction u of sub-motion `full`.
    struct Target {
        std::uint64_t full = 0;
        double u = 0;
        std::uint32_t op = 0; // the operation the tool belongs to there
        TimelinePosition pos{0, 0, 0.0};
    };

    void clear();
    Target endTarget() const;
    Target resolve(TimelinePosition p) const;
    Vec3 tipAt(const Target& t) const;
    // Cuts sub-motions [from, to) in batches (removed: run's per-motion output, or null).
    void cutRange(std::uint64_t from, std::uint64_t to, std::vector<double>* removed);
    // The end of the batch that starts at `from`, before `limit`, of at most `maxRays` of work (one motion at least).
    std::uint64_t batchEnd(std::uint64_t from, std::uint64_t limit, double maxRays, double* rays) const;
    void takeKeyframe(std::uint64_t motion);

    IJobSystem& jobs_;
    std::unique_ptr<IStockModel> stock_;
    bool cpu_ = false;
    SimTuning tuning_;

    // The last run.
    bool ready_ = false;
    std::vector<OpTable> ops_;
    std::vector<ToolShape> tools_;
    std::vector<Motion> motions_;
    std::vector<double> rays_; // per sub-motion
    std::vector<SimEvent> events_;
    std::vector<Keyframe> keyframes_;
    KeyframeLedger ledger_;        // what keyframes_ pin (CPU model)
    std::uint64_t stopMotion_ = 0; // the run cut [0, stopMotion_)
    std::uint32_t endOp_ = 0;
    TimelinePosition endPos_{0, 0, 0.0};
    std::uint64_t totalMoves_ = 0;
    Xform modelFromWcs_ = Xform::identity();
    double spacingRays_ = kKeyframeRays;
    std::uint64_t spacingMotions_ = 0;
    std::uint64_t budget_ = kKeyframeBudgetHighBytes;
    SimRunInfo info_;
    SeekReport report_;

    // Where the stock is now: applied_ sub-motions whole, and when partial_, part of the next one on top of clean_.
    std::uint64_t applied_ = 0;
    bool partial_ = false;
    std::shared_ptr<const IStockModel::Snapshot> clean_;
    TimelinePosition pos_{0, 0, 0.0};
    Vec3 tip_{};
};

void Simulator::clear()
{
    ready_ = false;
    ops_.clear();
    tools_.clear();
    motions_.clear();
    rays_.clear();
    events_.clear();
    keyframes_.clear();
    ledger_.clear();
    stopMotion_ = 0;
    endOp_ = 0;
    endPos_ = {0, 0, 0.0};
    totalMoves_ = 0;
    modelFromWcs_ = Xform::identity();
    info_ = SimRunInfo{};
    report_ = SeekReport{};
    applied_ = 0;
    partial_ = false;
    clean_.reset();
    pos_ = {0, 0, 0.0};
    tip_ = {};
}

std::uint64_t Simulator::batchEnd(std::uint64_t from, std::uint64_t limit, double maxRays, double* raysOut) const
{
    const std::uint32_t op = motions_[from].op;
    const std::size_t maxMotions = std::max<std::size_t>(1, tuning_.maxBatchMotions);
    double rays = 0;
    std::uint64_t j = from;
    while (j < limit && motions_[j].op == op && j - from < maxMotions) {
        if (j > from && rays + rays_[j] > maxRays) {
            break;
        }
        if (j > from && spacingMotions_ > 0 && j % spacingMotions_ == 0) {
            break; // a keyframe goes here
        }
        rays += rays_[j];
        ++j;
    }
    if (raysOut != nullptr) {
        *raysOut = rays;
    }
    return j;
}

void Simulator::cutRange(std::uint64_t from, std::uint64_t to, std::vector<double>* removed)
{
    std::vector<IStockModel::CutMove> moves;
    while (from < to) {
        const std::uint64_t j = batchEnd(from, to, tuning_.batchRays, nullptr);
        const ToolShape& shape = tools_[motions_[from].op];
        if (!shape.cuttingProfile.empty()) {
            moves.clear();
            for (std::uint64_t k = from; k < j; ++k) {
                moves.push_back({motions_[k].a, motions_[k].b, motions_[k].rapid});
            }
            std::span<double> out;
            if (removed != nullptr) {
                out = std::span<double>(removed->data() + from, static_cast<std::size_t>(j - from));
            }
            stock_->cutBatch(shape, moves, out);
        }
        from = j;
    }
}

void Simulator::takeKeyframe(std::uint64_t motion)
{
    const auto t0 = std::chrono::steady_clock::now();
    const auto since = [](std::chrono::steady_clock::time_point a) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
    };
    if (cpu_ && tuning_.internTiles) {
        // The tiles cut since the last keyframe, made one with any equal tile met before (tridexel.h, KEYFRAMES).
        internTiles(*stock_, keyframes_.empty() ? nullptr : keyframes_.back().snap.get());
        info_.internSeconds += since(t0);
    }
    keyframes_.push_back({motion, stock_->snapshot()});
    if (!cpu_) {
        info_.keyframeSeconds += since(t0);
        return;
    }
    ledger_.add(*keyframes_.back().snap);
    // Over budget: drop every other keyframe (the first and the last stay) and the spacing doubles; the ledger is
    // recounted from the ones kept.
    while (ledger_.pinnedBytes() > budget_ && keyframes_.size() > 2) {
        std::vector<Keyframe> kept;
        for (std::size_t k = 0; k + 1 < keyframes_.size(); k += 2) {
            kept.push_back(keyframes_[k]);
        }
        kept.push_back(keyframes_.back());
        keyframes_ = std::move(kept);
        std::vector<const IStockModel::Snapshot*> snaps;
        for (const Keyframe& k : keyframes_) {
            snaps.push_back(k.snap.get());
        }
        ledger_.rebuild(snaps);
        spacingRays_ *= 2.0;
        spacingMotions_ *= 2;
        ++info_.thinnings;
    }
    info_.keyframeSeconds += since(t0);
}

Status Simulator::run(const SimProgram& p, const SimOptions& opts, JobContext& ctx)
{
    clear();
    const auto fail = [&](Error e) -> Status {
        clear();
        return tl::unexpected(std::move(e));
    };
    const Box3 bounds = soupBox(p.stockSolid);
    if (p.stockSolid.indices.empty() || !(bounds.max.x > bounds.min.x) || !(bounds.max.y > bounds.min.y) ||
        !(bounds.max.z > bounds.min.z)) {
        return fail(error(ErrorCode::InvalidArgument, "The setup has no stock solid to simulate."));
    }
    const double wChecks = opts.safetyChecks ? 0.3 : 0.0;
    const double wGouges = opts.gouges ? 0.1 : 0.0;
    Progress checksProgress = ctx.progress.sub(wChecks);
    Progress gougeProgress = ctx.progress.sub(wGouges);
    Progress cutProgress = ctx.progress.sub(1.0 - wChecks - wGouges);

    const VerifyInput in = verifyInputFrom(p);
    motions_ = expandMotions(in);
    modelFromWcs_ = p.modelFromWcs;
    const Vec3 cell = snappedCell(bounds, opts.resolution.cellMm);

    // The tables of the operations.
    ops_.resize(p.ops.size());
    tools_.resize(p.ops.size());
    for (std::size_t k = 0; k < p.ops.size(); ++k) {
        OpTable& o = ops_[k];
        o.id = in.ops[k].id;
        if (k < p.tools.size()) {
            tools_[k] = p.tools[k];
        }
        o.radius = maxRadius(tools_[k].cuttingProfile);
        o.height = tools_[k].cuttingProfile.empty() ? 0.0 : tools_[k].cuttingProfile.back().y;
        const cam::Toolpath* tp = in.ops[k].path;
        if (tp != nullptr) {
            o.hasPath = true;
            o.moves = static_cast<std::uint32_t>(tp->moves.size());
            o.start = {toMm(tp->startX), toMm(tp->startY), toMm(tp->startZ)};
            totalMoves_ += o.moves;
        }
    }
    // Sub-motions are in program order, each operation's by move: one pass fills the move tables.
    {
        std::uint64_t i = 0;
        for (std::size_t k = 0; k < ops_.size(); ++k) {
            OpTable& o = ops_[k];
            o.firstMotion = i;
            if (o.hasPath) {
                o.moveFirst.assign(static_cast<std::size_t>(o.moves) + 1, 0);
                for (std::uint32_t m = 0; m <= o.moves; ++m) {
                    while (i < motions_.size() && motions_[i].op == k && motions_[i].move < m) {
                        ++i;
                    }
                    o.moveFirst[m] = i;
                }
                while (i < motions_.size() && motions_[i].op == k) {
                    ++i;
                }
                o.moveFirst[o.moves] = i;
            }
            o.endMotion = i;
        }
    }
    rays_.resize(motions_.size());
    for (std::size_t i = 0; i < motions_.size(); ++i) {
        const OpTable& o = ops_[motions_[i].op];
        rays_[i] = rayCost(motions_[i], o.radius, o.height, bounds, cell);
    }

    // Events: the checks, then the gouges.
    struct Tagged {
        std::uint32_t op;
        int group; // -1 the setup's (before every move's), 0 the checks, 1 gouges
        SimEvent e;
    };
    std::vector<Tagged> tagged;
    if (opts.safetyChecks) {
        JobContext sub{ctx.cancel, checksProgress};
        Result<std::vector<SimEvent>> checks = verifyProgram(in, jobs_, sub);
        if (!checks) {
            return fail(checks.error());
        }
        // The checks' events come in operation order; find each one's operation by its id from where the last was.
        std::size_t cursor = 0;
        for (SimEvent& e : *checks) {
            if (e.moveIndex == kSetupEvent) {
                tagged.push_back({0, -1, std::move(e)}); // no operation, no move
                continue;
            }
            std::size_t k = cursor;
            while (k < ops_.size() && ops_[k].id != e.operation) {
                ++k;
            }
            if (k == ops_.size()) {
                k = cursor;
            }
            cursor = k;
            tagged.push_back({static_cast<std::uint32_t>(k), 0, std::move(e)});
        }
    }
    if (opts.gouges) {
        JobContext sub{ctx.cancel, gougeProgress};
        GougeStats gs;
        Result<std::vector<GougeEvent>> g = findGouges(in, motions_, opts.gougeTolMm, jobs_, sub, &gs);
        if (!g) {
            return fail(g.error());
        }
        info_.gougeMotionsTested = gs.tested;
        info_.gougeMotionsFlagged = gs.flagged;
        for (GougeEvent& ge : *g) {
            tagged.push_back({ge.op, 1, std::move(ge.event)});
        }
    }
    info_.gougeTolMm = std::max(opts.gougeTolMm, kGougeMinTolMm);
    std::stable_sort(tagged.begin(), tagged.end(), [](const Tagged& a, const Tagged& b) {
        if ((a.group < 0) != (b.group < 0)) {
            return a.group < 0; // the setup's first
        }
        if (a.op != b.op) {
            return a.op < b.op;
        }
        if (a.e.moveIndex != b.e.moveIndex) {
            return a.e.moveIndex < b.e.moveIndex;
        }
        return a.group < b.group;
    });

    // Where the run ends.
    stopMotion_ = motions_.size();
    endOp_ = 0;
    endPos_ = {0, 0, 0.0};
    for (std::size_t k = ops_.size(); k-- > 0;) {
        if (ops_[k].moves > 0) {
            endOp_ = static_cast<std::uint32_t>(k);
            endPos_ = {static_cast<std::uint32_t>(k), ops_[k].moves - 1, 1.0};
            break;
        }
    }
    if (opts.stopOnError) {
        for (std::size_t e = 0; e < tagged.size(); ++e) {
            const Tagged& t = tagged[e];
            if (t.e.severity != Severity::Error || t.op >= ops_.size() || !ops_[t.op].hasPath ||
                t.e.moveIndex >= ops_[t.op].moves) {
                continue;
            }
            const std::uint32_t op = t.op, move = t.e.moveIndex;
            stopMotion_ = ops_[op].moveFirst[static_cast<std::size_t>(move) + 1];
            endOp_ = op;
            endPos_ = {op, move, 1.0};
            std::size_t keep = e;
            while (keep < tagged.size() && tagged[keep].op == op && tagged[keep].e.moveIndex == move) {
                ++keep;
            }
            tagged.resize(keep);
            break;
        }
    }
    events_.reserve(tagged.size());
    for (Tagged& t : tagged) {
        events_.push_back(std::move(t.e));
    }

    // The material removal.
    stock_->reset(p.stockSolid, bounds, opts.resolution);
    spacingRays_ = tuning_.keyframeRays > 0 ? tuning_.keyframeRays : kKeyframeRays;
    spacingMotions_ = tuning_.keyframeMotions;
    budget_ = tuning_.keyframeBudgetBytes > 0 ? tuning_.keyframeBudgetBytes : keyframeBudgetFor(opts.resolution.cellMm);
    info_.keyframeBudgetBytes = budget_;
    std::vector<double> removed(static_cast<std::size_t>(stopMotion_), 0.0);
    takeKeyframe(0);
    double sinceKey = 0;
    std::uint64_t i = 0;
    std::vector<IStockModel::CutMove> moves;
    // The wall time of each batch's cut (its end motion, seconds), for seekReport only: nothing else reads it.
    std::vector<std::pair<std::uint64_t, double>> batchSeconds;
    while (i < stopMotion_) {
        if (Status s = ctx.cancel.check(); !s) {
            return fail(s.error());
        }
        cutProgress.set(static_cast<double>(i) / static_cast<double>(stopMotion_), "Simulating material removal");
        double rays = 0;
        // A batch ends at twice the spacing from the last keyframe at the latest, so a seek recuts at most that (or
        // one motion).
        const double room = spacingMotions_ > 0 ? tuning_.batchRays : 2.0 * spacingRays_ - sinceKey;
        const std::uint64_t j = batchEnd(i, stopMotion_, std::min(tuning_.batchRays, room), &rays);
        const ToolShape& shape = tools_[motions_[i].op];
        if (!shape.cuttingProfile.empty()) {
            moves.clear();
            for (std::uint64_t k = i; k < j; ++k) {
                moves.push_back({motions_[k].a, motions_[k].b, motions_[k].rapid});
            }
            const auto t0 = std::chrono::steady_clock::now();
            stock_->cutBatch(shape, moves, std::span<double>(removed.data() + i, static_cast<std::size_t>(j - i)));
            batchSeconds.emplace_back(j, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            info_.cutSeconds += batchSeconds.back().second;
        }
        ++info_.batches;
        sinceKey += rays;
        i = j;
        const bool due = spacingMotions_ > 0 ? i % spacingMotions_ == 0 : sinceKey >= spacingRays_;
        if (due && i < stopMotion_) {
            takeKeyframe(i);
            sinceKey = 0;
        }
    }
    if (keyframes_.back().motion != stopMotion_) {
        takeKeyframe(stopMotion_);
    }
    cutProgress.set(1.0, "Simulating material removal");

    info_.motions = motions_.size();
    info_.runMotions = stopMotion_;
    info_.removedMm3 = std::move(removed);
    info_.motionOp.reserve(motions_.size());
    info_.motionMove.reserve(motions_.size());
    for (const Motion& m : motions_) {
        info_.motionOp.push_back(m.op);
        info_.motionMove.push_back(m.move);
    }
    info_.keyframeRays = spacingRays_;
    info_.cellMm = opts.resolution.cellMm;
    // The seek report: the longest keyframe interval as the run timed its cutting (batches end at keyframes, so each
    // batch lies in one interval: the one its end motion closes).
    report_.thinned = info_.thinnings > 0;
    {
        double worst = 0, sum = 0;
        std::size_t k = 1; // the interval [keyframes_[k - 1], keyframes_[k])
        for (const auto& [end, sec] : batchSeconds) {
            while (k + 1 < keyframes_.size() && end > keyframes_[k].motion) {
                worst = std::max(worst, sum);
                sum = 0;
                ++k;
            }
            sum += sec;
        }
        report_.worstSeekMs = 1000.0 * std::max(worst, sum);
    }
    applied_ = stopMotion_;
    const Target end = endTarget();
    pos_ = end.pos;
    tip_ = tipAt(end);
    ready_ = true;
    ctx.progress.set(1.0, "Simulating material removal");
    return {};
}

Simulator::Target Simulator::endTarget() const
{
    Target t;
    t.full = stopMotion_;
    t.op = endOp_;
    t.pos = endPos_;
    return t;
}

Simulator::Target Simulator::resolve(TimelinePosition p) const
{
    if (ops_.empty() || p.opIndex >= ops_.size()) {
        return endTarget();
    }
    Target t;
    t.op = p.opIndex;
    const OpTable& o = ops_[p.opIndex];
    if (!o.hasPath || o.moves == 0) {
        t.full = o.firstMotion;
        t.pos = {p.opIndex, 0, 0.0};
    } else if (p.moveIndex >= o.moves) {
        t.full = o.endMotion;
        t.pos = {p.opIndex, o.moves - 1, 1.0};
    } else {
        const double f = std::isfinite(p.t01) ? std::clamp(p.t01, 0.0, 1.0) : 0.0;
        t.pos = {p.opIndex, p.moveIndex, f};
        const std::uint64_t f0 = o.moveFirst[p.moveIndex], f1 = o.moveFirst[p.moveIndex + 1];
        double total = 0;
        for (std::uint64_t k = f0; k < f1; ++k) {
            total += length(motions_[k]);
        }
        const double target = f * total;
        double cum = 0;
        t.full = f1;
        for (std::uint64_t k = f0; k < f1; ++k) {
            const double len = length(motions_[k]);
            if (cum + len > target) {
                t.full = k;
                t.u = len > 0 ? std::clamp((target - cum) / len, 0.0, 1.0) : 0.0;
                if (!(t.u < 1.0)) {
                    t.full = k + 1;
                    t.u = 0;
                }
                break;
            }
            cum += len;
        }
    }
    if (t.full > stopMotion_ || (t.full == stopMotion_ && t.u > 0)) {
        return endTarget();
    }
    return t;
}

Vec3 Simulator::tipAt(const Target& t) const
{
    if (t.u > 0 && t.full < motions_.size()) {
        return lerp(motions_[t.full].a, motions_[t.full].b, t.u);
    }
    if (t.op < ops_.size()) {
        const OpTable& o = ops_[t.op];
        if (t.full > o.firstMotion && t.full <= motions_.size() && motions_[t.full - 1].op == t.op) {
            return motions_[t.full - 1].b;
        }
        if (o.hasPath) {
            return o.start;
        }
    }
    return t.full > 0 && t.full <= motions_.size() ? motions_[t.full - 1].b : Vec3{};
}

Status Simulator::seek(TimelinePosition p)
{
    if (!ready_) {
        return tl::unexpected(error(ErrorCode::InvalidArgument, "No simulation has been run."));
    }
    const Target t = resolve(p);
    if (partial_) {
        stock_->restore(*clean_);
        clean_.reset();
        partial_ = false;
    }
    // The last keyframe at or before the target.
    std::size_t k = keyframes_.size();
    while (k > 0 && keyframes_[k - 1].motion > t.full) {
        --k;
    }
    const Keyframe& key = keyframes_[k > 0 ? k - 1 : 0];
    if (applied_ > t.full || key.motion > applied_) {
        stock_->restore(*key.snap);
        applied_ = key.motion;
    }
    cutRange(applied_, t.full, nullptr);
    applied_ = t.full;
    if (t.u > 0 && t.full < motions_.size()) {
        const Motion& m = motions_[t.full];
        const ToolShape& shape = tools_[m.op];
        if (!shape.cuttingProfile.empty()) {
            clean_ = stock_->snapshot();
            partial_ = true;
            stock_->cut(shape, m.a, lerp(m.a, m.b, t.u), m.rapid);
        }
    }
    pos_ = t.pos;
    tip_ = tipAt(t);
    return {};
}

Xform Simulator::toolPose() const
{
    // modelFromWcs, then the tip: the tool axis is the WCS's +Z and the glyph's tip is at its origin.
    Xform x = modelFromWcs_;
    const double* m = modelFromWcs_.m;
    x.m[3] = m[0] * tip_.x + m[1] * tip_.y + m[2] * tip_.z + m[3];
    x.m[7] = m[4] * tip_.x + m[5] * tip_.y + m[6] * tip_.z + m[7];
    x.m[11] = m[8] * tip_.x + m[9] * tip_.y + m[10] * tip_.z + m[11];
    return x;
}

kernel::TriSoup Simulator::finalStock() const
{
    if (!ready_ || keyframes_.empty()) {
        return {};
    }
    if (applied_ == stopMotion_ && !partial_) {
        return stock_->toSolid();
    }
    const std::unique_ptr<IStockModel> copy = stock_->clone();
    copy->restore(*keyframes_.back().snap);
    return copy->toSolid();
}

void setSimTuning(ISimulator& sim, const SimTuning& tuning)
{
    dynamic_cast<Simulator&>(sim).setTuning(tuning);
}

SimRunInfo simRunInfo(const ISimulator& sim)
{
    return dynamic_cast<const Simulator&>(sim).info();
}

} // namespace detail

std::unique_ptr<ISimulator> makeSimulator(IJobSystem& jobs, std::unique_ptr<IStockModel> stock)
{
    return std::make_unique<detail::Simulator>(jobs, std::move(stock));
}

} // namespace ray::sim
