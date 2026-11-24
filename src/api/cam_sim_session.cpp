// The on-screen simulation of one setup (ray_api/cam_sim_session.h, task T-074).
#include <ray_api/cam_sim_session.h>

#include <ray/cam.h>
#include <ray/sim.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace ray::api::cam_sim {

namespace {

tl::unexpected<Error> failure(ErrorCode code, std::string message)
{
    return tl::unexpected(Error{code, std::move(message), {}});
}

double msSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// The rates the timeline's moves run at (see the header): cam.h's cycle-time defaults where the machine is silent.
struct Rates {
    double rapid[3] = {cam::kCycleTimeDefaultRapidMmMin, cam::kCycleTimeDefaultRapidMmMin,
                       cam::kCycleTimeDefaultRapidMmMin};
    double maxFeed = 0; // 0: not known (the programmed feed runs as it is)
};

Rates ratesOf(const lib::MachineDef* machine)
{
    Rates r;
    if (machine == nullptr)
        return r;
    for (const lib::AxisDef& a : machine->axes) {
        const int i = a.letter == 'X' ? 0 : a.letter == 'Y' ? 1 : a.letter == 'Z' ? 2 : -1;
        if (i < 0)
            continue;
        if (a.rapidMmMin > 0)
            r.rapid[i] = a.rapidMmMin;
        if (a.maxFeedMmMin > 0)
            r.maxFeed = r.maxFeed > 0 ? std::min(r.maxFeed, a.maxFeedMmMin) : a.maxFeedMmMin;
    }
    return r;
}

double segmentSec(const sim::BackplotVertex& a, const sim::BackplotVertex& b, const Rates& rates)
{
    const double dx = double(b.x) - double(a.x), dy = double(b.y) - double(a.y), dz = double(b.z) - double(a.z);
    if (static_cast<cam::MoveKind>(b.kind) == cam::MoveKind::Rapid || b.feed <= 0) {
        if (static_cast<cam::MoveKind>(b.kind) != cam::MoveKind::Rapid)
            return 0; // no motion (a dwell's or a cancel's vertex)
        // Each axis at its own rapid rate: the slowest decides (a dogleg G0).
        return 60.0 *
               std::max({std::abs(dx) / rates.rapid[0], std::abs(dy) / rates.rapid[1], std::abs(dz) / rates.rapid[2]});
    }
    const double feed = rates.maxFeed > 0 ? std::min(double(b.feed), rates.maxFeed) : double(b.feed);
    return 60.0 * std::sqrt(dx * dx + dy * dy + dz * dz) / feed;
}

double length(const sim::BackplotVertex& a, const sim::BackplotVertex& b)
{
    const double dx = double(b.x) - double(a.x), dy = double(b.y) - double(a.y), dz = double(b.z) - double(a.z);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool isCycle(cam::MoveKind k)
{
    switch (k) {
    case cam::MoveKind::CycleDrill:
    case cam::MoveKind::CyclePeck:
    case cam::MoveKind::CycleChipBreak:
    case cam::MoveKind::CycleTap:
    case cam::MoveKind::CycleBore:
    case cam::MoveKind::CycleReam:
        return true;
    default:
        return false;
    }
}

// Where along move `m` (its vertices in `bp`) the path comes nearest `p`, as a fraction of its length.
double nearestFraction(const sim::Backplot& bp, std::uint32_t m, const Vec3& p)
{
    const std::uint32_t first = bp.moveFirstVertex[m], last = bp.moveFirstVertex[m + 1];
    double total = 0;
    for (std::uint32_t v = std::max(first, 1u); v < last; ++v)
        total += length(bp.lineVertices[v - 1], bp.lineVertices[v]);
    if (total <= 0)
        return 0;
    double best = std::numeric_limits<double>::infinity(), bestAt = 0, run = 0;
    for (std::uint32_t v = std::max(first, 1u); v < last; ++v) {
        const sim::BackplotVertex& a = bp.lineVertices[v - 1];
        const sim::BackplotVertex& b = bp.lineVertices[v];
        const double len = length(a, b);
        const double ax = a.x, ay = a.y, az = a.z;
        const double dx = double(b.x) - ax, dy = double(b.y) - ay, dz = double(b.z) - az;
        double s = 0;
        if (len > 0)
            s = std::clamp(((p.x - ax) * dx + (p.y - ay) * dy + (p.z - az) * dz) / (len * len), 0.0, 1.0);
        const double qx = ax + s * dx - p.x, qy = ay + s * dy - p.y, qz = az + s * dz - p.z;
        const double d = qx * qx + qy * qy + qz * qz;
        if (d < best) {
            best = d;
            bestAt = run + s * len;
        }
        run += len;
    }
    return std::clamp(bestAt / total, 0.0, 1.0);
}

double area(const Box3& b)
{
    return std::max(0.0, b.max.x - b.min.x) * std::max(0.0, b.max.y - b.min.y);
}

Box3 boundsOf(const kernel::TriSoup& soup)
{
    Box3 b{{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::infinity()},
           {-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity()}};
    for (std::size_t i = 0; i + 2 < soup.positions.size(); i += 3) {
        b.min.x = std::min(b.min.x, soup.positions[i]);
        b.min.y = std::min(b.min.y, soup.positions[i + 1]);
        b.min.z = std::min(b.min.z, soup.positions[i + 2]);
        b.max.x = std::max(b.max.x, soup.positions[i]);
        b.max.y = std::max(b.max.y, soup.positions[i + 1]);
        b.max.z = std::max(b.max.z, soup.positions[i + 2]);
    }
    return b;
}

} // namespace

// ---------- Timeline ----------
sim::TimelinePosition Timeline::normalised(const sim::TimelinePosition& p) const
{
    if (ops.empty())
        return {0, 0, 0};
    if (p.opIndex >= ops.size())
        return end();
    const TimelineOperation& op = ops[p.opIndex];
    if (op.moves() == 0)
        return {p.opIndex, 0, 0};
    if (p.moveIndex >= op.moves())
        return {p.opIndex, static_cast<std::uint32_t>(op.moves() - 1), 1};
    const double t = std::isfinite(p.t01) ? std::clamp(p.t01, 0.0, 1.0) : 0.0;
    return {p.opIndex, p.moveIndex, t};
}

sim::TimelinePosition Timeline::start() const
{
    return {0, 0, 0};
}

sim::TimelinePosition Timeline::end() const
{
    for (std::size_t i = ops.size(); i-- > 0;) {
        if (ops[i].moves() > 0)
            return {static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(ops[i].moves() - 1), 1};
    }
    return {0, 0, 0};
}

double Timeline::secondsAt(const sim::TimelinePosition& pos) const
{
    if (ops.empty())
        return 0;
    const sim::TimelinePosition p = normalised(pos);
    const TimelineOperation& op = ops[p.opIndex];
    if (op.moves() == 0)
        return p.t01 > 0 ? op.endSec : op.startSec;
    const double s0 = p.moveIndex == 0 ? op.firstMoveSec() : op.moveEndSec[p.moveIndex - 1];
    const double s1 = op.moveEndSec[p.moveIndex];
    if (p.moveIndex == 0 && p.t01 <= 0)
        return op.startSec; // the start of an operation is the start of its tool change
    return s0 + p.t01 * (s1 - s0);
}

sim::TimelinePosition Timeline::positionAt(double sec) const
{
    if (ops.empty())
        return {0, 0, 0};
    if (!(sec > 0))
        return start();
    if (sec >= totalSec)
        return end();
    // The last operation that has begun by then.
    std::size_t i = 0;
    for (std::size_t k = 0; k < ops.size(); ++k) {
        if (ops[k].startSec <= sec)
            i = k;
    }
    const TimelineOperation& op = ops[i];
    const auto index = static_cast<std::uint32_t>(i);
    if (op.moves() == 0 || sec <= op.firstMoveSec())
        return {index, 0, 0};
    const auto it = std::upper_bound(op.moveEndSec.begin(), op.moveEndSec.end(), sec);
    if (it == op.moveEndSec.end())
        return {index, static_cast<std::uint32_t>(op.moves() - 1), 1};
    const auto m = static_cast<std::uint32_t>(it - op.moveEndSec.begin());
    const double s0 = m == 0 ? op.firstMoveSec() : op.moveEndSec[m - 1];
    const double s1 = op.moveEndSec[m];
    return {index, m, s1 > s0 ? std::clamp((sec - s0) / (s1 - s0), 0.0, 1.0) : 1.0};
}

std::uint64_t Timeline::movesDone(const sim::TimelinePosition& pos) const
{
    if (ops.empty())
        return 0;
    const sim::TimelinePosition p = normalised(pos);
    const TimelineOperation& op = ops[p.opIndex];
    if (op.moves() == 0)
        return op.firstMove;
    return op.firstMove + p.moveIndex + (p.t01 >= 1 ? 1 : 0);
}

sim::TimelinePosition Timeline::afterMoves(std::uint64_t done) const
{
    if (done == 0 || ops.empty())
        return start();
    if (done >= moveCount)
        return end();
    const std::uint64_t f = done - 1; // the last move done
    for (std::size_t i = 0; i < ops.size(); ++i) {
        const TimelineOperation& op = ops[i];
        if (f >= op.firstMove && f < op.firstMove + op.moves())
            return {static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(f - op.firstMove), 1};
    }
    return end();
}

sim::TimelinePosition Timeline::step(const sim::TimelinePosition& pos, int moves) const
{
    if (moves == 0 || moveCount == 0)
        return normalised(pos);
    const sim::TimelinePosition p = normalised(pos);
    const std::uint64_t done = movesDone(p);
    const bool inside = p.t01 > 0 && p.t01 < 1;
    std::int64_t target = static_cast<std::int64_t>(done) + moves;
    if (moves < 0 && inside)
        target += 1; // back to the start of the move it is in first
    target = std::clamp<std::int64_t>(target, 0, static_cast<std::int64_t>(moveCount));
    return afterMoves(static_cast<std::uint64_t>(target));
}

Timeline buildTimeline(const sim::SimProgram& program, const cam::CycleTime* estimate,
                       std::span<const sim::SimEvent> events, std::vector<EventPlace>* places)
{
    Timeline t;
    const Rates rates = ratesOf(program.machine);
    if (places != nullptr)
        places->assign(events.size(), EventPlace{});
    std::vector<bool> placed(events.size(), false);
    double clock = 0;
    for (std::size_t i = 0; i < program.ops.size(); ++i) {
        TimelineOperation op;
        op.id = program.ops[i] ? program.ops[i]->id : OperationId{};
        op.name = program.ops[i] ? program.ops[i]->name : std::string();
        op.firstMove = t.moveCount;
        op.startSec = clock;
        const cam::Toolpath* path = i < program.toolpaths.size() ? program.toolpaths[i].get() : nullptr;
        const cam::CycleTimePart* part =
            estimate != nullptr && i < estimate->operations.size() ? &estimate->operations[i] : nullptr;
        op.toolChangeSec = part ? std::max(0.0, part->toolChangeSec) : 0.0;
        if (path != nullptr && !path->moves.empty()) {
            const sim::Backplot bp = sim::buildBackplot(*path, Xform::identity(), 0.01, program.machine);
            const std::size_t n = path->moves.size();
            std::vector<double> own(n, 0.0);
            double sum = 0;
            for (std::size_t m = 0; m < n && m + 1 < bp.moveFirstVertex.size(); ++m) {
                double s = 0;
                for (std::uint32_t v = std::max(bp.moveFirstVertex[m], 1u); v < bp.moveFirstVertex[m + 1]; ++v)
                    s += segmentSec(bp.lineVertices[v - 1], bp.lineVertices[v], rates);
                const cam::Move& mv = path->moves[m];
                if (mv.kind == cam::MoveKind::Dwell || isCycle(mv.kind))
                    s += std::max(0, mv.cycleDwellMs) / 1000.0;
                own[m] = s;
                sum += s;
            }
            // Scaled to the estimate of the operation (without its tool change).
            double scale = 1;
            if (part != nullptr) {
                const double motion = part->totalSec - part->toolChangeSec;
                if (motion > 0 && sum > 0) {
                    scale = motion / sum;
                    t.scaledToEstimate = true;
                }
            }
            op.moveEndSec.resize(n);
            double at = op.firstMoveSec();
            for (std::size_t m = 0; m < n; ++m) {
                at += own[m] * scale;
                op.moveEndSec[m] = at;
            }
            // The events of this operation.
            for (std::size_t e = 0; e < events.size(); ++e) {
                if (placed[e] || events[e].operation != op.id)
                    continue;
                const std::uint32_t mi =
                    std::min<std::uint32_t>(events[e].moveIndex, static_cast<std::uint32_t>(n - 1));
                EventPlace pl;
                pl.opIndex = static_cast<std::uint32_t>(i);
                pl.moveIndex = mi;
                pl.t01 = mi + 1 < bp.moveFirstVertex.size() ? nearestFraction(bp, mi, events[e].position) : 0;
                if (places != nullptr)
                    (*places)[e] = pl;
                placed[e] = true;
            }
        }
        op.endSec = op.moveEndSec.empty() ? op.firstMoveSec() : op.moveEndSec.back();
        clock = op.endSec;
        t.moveCount += op.moves();
        t.ops.push_back(std::move(op));
    }
    t.totalSec = clock;
    if (places != nullptr) {
        for (std::size_t e = 0; e < places->size(); ++e) {
            EventPlace& pl = (*places)[e];
            pl.sec = t.secondsAt({pl.opIndex, pl.moveIndex, pl.t01});
        }
    }
    return t;
}

// ---------- SimSession ----------
struct SimSession::Impl {
    IJobSystem* jobs = nullptr;
    std::unique_ptr<sim::ISimulator> simulator;
    Box3 stockBounds{};
    bool ready = false;
};

SimSession::SimSession(IJobSystem& jobs) : impl_(std::make_unique<Impl>())
{
    impl_->jobs = &jobs;
}

SimSession::~SimSession() = default;

Frame SimSession::frameNow(const MeshAsk& ask)
{
    Frame f;
    const sim::ISimulator& s = *impl_->simulator;
    f.position = s.position();
    f.toolPose = s.toolPose();
    const sim::IStockModel& stock = s.stock();
    f.version = stock.version();
    const auto t0 = std::chrono::steady_clock::now();
    const std::uint32_t budget = std::max<std::uint32_t>(ask.budget, 1000);
    if (ask.full) {
        Box3 box;
        f.mesh = std::make_shared<const kernel::Mesh>(stock.toMesh(0, &box, budget));
        f.full = true;
    } else if (ask.shownVersion != f.version) {
        // The dirty box first, at the coarsest level, then that box at the display's level of detail.
        Box3 probe;
        (void)stock.toMesh(ask.shownVersion, &probe, 1);
        if (probe.min.x <= probe.max.x && probe.min.y <= probe.max.y) {
            const double whole = area(impl_->stockBounds);
            const double share = whole > 0 ? std::clamp(area(probe) / whole, 0.0, 1.0) : 1.0;
            auto want = static_cast<std::uint32_t>(std::max(64.0, std::ceil(budget * share)));
            if (ask.cap > 0 && want > ask.cap) {
                want = ask.cap;
                f.coarse = true;
            }
            Box3 dirty;
            f.mesh = std::make_shared<const kernel::Mesh>(stock.toMesh(ask.shownVersion, &dirty, want));
            f.dirty = dirty;
        }
    }
    if (f.mesh)
        f.triangles = f.mesh->indices.size() / 3;
    f.meshMs = msSince(t0);
    return f;
}

Result<std::shared_ptr<RunResult>> SimSession::run(const RunRequest& request, JobContext& ctx)
{
    try {
        impl_->ready = false;
        auto out = std::make_shared<RunResult>();
        out->setup = request.setup;
        out->cellMm = request.cellMm;
        // 1. The program (its kernel work happens here, on this worker).
        auto t0 = std::chrono::steady_clock::now();
        ProgramOptions po;
        po.startStock = request.startStock.get();
        Progress building = ctx.progress.sub(0.1);
        building.set(0, "Preparing the program");
        JobContext buildCtx{ctx.cancel, building};
        auto data = buildSimProgram(request.state, request.build, request.setup, request.toolpaths, po, buildCtx);
        if (!data)
            return tl::unexpected(data.error());
        const sim::SimProgram& program = data->program;
        out->programSec = msSince(t0) / 1000.0;
        std::uint64_t moves = 0;
        for (const auto& p : program.toolpaths)
            moves += p ? p->moves.size() : 0;
        if (moves == 0)
            return failure(ErrorCode::InvalidArgument, "The setup has no moves to simulate");
        if (auto ok = ctx.cancel.check(); !ok)
            return tl::unexpected(ok.error());

        // 2. The run.
        t0 = std::chrono::steady_clock::now();
        impl_->simulator = sim::makeSimulator(*impl_->jobs, nullptr);
        if (!impl_->simulator)
            return failure(ErrorCode::Internal, "The simulator could not start");
        sim::SimOptions options;
        options.resolution = sim::StockResolution{request.cellMm};
        options.gougeTolMm = request.gougeTolMm;
        Progress running = ctx.progress.sub(0.85);
        JobContext runCtx{ctx.cancel, running};
        if (auto st = impl_->simulator->run(program, options, runCtx); !st)
            return tl::unexpected(st.error());
        out->runSec = msSince(t0) / 1000.0;
        if (auto ok = ctx.cancel.check(); !ok)
            return tl::unexpected(ok.error());

        // 3. The timeline, on the CAM tree's cycle-time estimate.
        t0 = std::chrono::steady_clock::now();
        Progress timing = ctx.progress.sub(0.05);
        timing.set(0, "Timeline");
        const auto events = impl_->simulator->events();
        out->events.assign(events.begin(), events.end());
        std::optional<cam::CycleTime> estimate;
        if (program.machine != nullptr) {
            std::vector<cam::CycleTimeOperation> ops;
            for (std::size_t i = 0; i < program.ops.size(); ++i) {
                const int number = i < request.toolNumbers.size() ? request.toolNumbers[i]
                                   : program.ops[i] != nullptr    ? program.ops[i]->tool.toolNumber
                                                                  : 0;
                ops.push_back({program.toolpaths[i].get(), number});
            }
            estimate = cam::estimateCycleTime(ops, *program.machine);
        }
        out->timeline = buildTimeline(program, estimate ? &*estimate : nullptr, out->events, &out->places);
        out->timelineSec = msSince(t0) / 1000.0;
        timing.set(1);

        // What the UI keeps of the program.
        const cam::Setup* setup = program.setup;
        out->setupName = setup ? setup->name : std::string();
        for (std::size_t i = 0; i < program.ops.size(); ++i) {
            out->ops.push_back(program.ops[i] ? program.ops[i]->id : OperationId{});
            out->opNames.push_back(program.ops[i] ? program.ops[i]->name : std::string());
            out->inputs.push_back(program.toolpaths[i] ? program.toolpaths[i]->inputHash : Hash256{});
        }
        out->tools = program.tools;
        out->modelFromWcs = program.modelFromWcs;
        out->stockBounds = boundsOf(program.stockSolid);
        impl_->stockBounds = out->stockBounds;
        impl_->ready = true;

        // 4. The display at the run's end.
        MeshAsk first = request.mesh;
        first.full = true;
        out->first = frameNow(first);
        return out;
    } catch (const std::bad_alloc&) {
        impl_->ready = false;
        return failure(ErrorCode::Internal, "Out of memory while simulating");
    }
}

Result<Frame> SimSession::seek(const sim::TimelinePosition& to, const MeshAsk& ask)
{
    if (!impl_->ready || !impl_->simulator)
        return failure(ErrorCode::InvalidArgument, "Nothing has been simulated yet");
    try {
        const auto t0 = std::chrono::steady_clock::now();
        if (auto st = impl_->simulator->seek(to); !st)
            return tl::unexpected(st.error());
        const double seekMs = msSince(t0);
        Frame f = frameNow(ask);
        f.seekMs = seekMs;
        return f;
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Out of memory while simulating");
    }
}

Result<Frame> SimSession::refresh(const MeshAsk& ask)
{
    if (!impl_->ready || !impl_->simulator)
        return failure(ErrorCode::InvalidArgument, "Nothing has been simulated yet");
    try {
        return frameNow(ask);
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Out of memory while simulating");
    }
}

} // namespace ray::api::cam_sim
