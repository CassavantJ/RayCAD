// Host motion services: arc splitting and linearisation, canned-cycle expansion and cycle runs (engine_motion.h).
// Deterministic: integer nanometres in and out, trigonometry through ray::dmath, no platform libm.
#include "engine_motion.h"

#include "format.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace ray::post {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

using cam::ArcPlane;
using cam::Move;
using cam::MoveKind;
using cam::MoveRole;

// Plane axes as indices into {x, y, z}: u and v span the plane right-handed about the normal w.
struct PlaneAxes {
    int u, v, w;
};
PlaneAxes axesOf(ArcPlane plane)
{
    switch (plane) {
    case ArcPlane::XZ:
        return {2, 0, 1}; // G18: Z x X = +Y
    case ArcPlane::YZ:
        return {1, 2, 0}; // G19: Y x Z = +X
    case ArcPlane::XY:
    default:
        return {0, 1, 2};
    }
}

struct Point {
    std::array<Nm, 3> c{};
    Nm& operator[](int axis) { return c[static_cast<std::size_t>(axis)]; }
    Nm operator[](int axis) const { return c[static_cast<std::size_t>(axis)]; }
    bool operator==(const Point&) const = default;
};

Point endOf(const Move& m)
{
    return Point{{m.x, m.y, m.z}};
}

Error failure(std::string message)
{
    return Error{ErrorCode::PostFailed, std::move(message), {}};
}

std::string moveLabel(std::size_t index)
{
    return "move " + std::to_string(index);
}

Nm roundToNm(double value)
{
    return static_cast<Nm>(std::llround(value));
}

// start + (end - start) * k / n, rounded half to even, in integers.
Nm lerpNm(Nm start, Nm end, std::int64_t k, std::int64_t n)
{
    return start + fmt::divRoundHalfEven((end - start) * k, n);
}

struct ArcFrame {
    PlaneAxes axes;
    double a0 = 0;     // start angle
    double sweep = 0;  // radians, (0, 2 pi]
    double radius = 0; // nm
    bool full = false;
    double direction = 1; // +1 counter-clockwise, -1 clockwise
    Nm cu = 0, cv = 0;
};

ArcFrame frameOf(const Move& arc, const Point& start)
{
    ArcFrame f;
    f.axes = axesOf(arc.plane);
    const Point centre{{arc.i, arc.j, arc.k}};
    f.cu = centre[f.axes.u];
    f.cv = centre[f.axes.v];
    const double su = static_cast<double>(start[f.axes.u] - f.cu);
    const double sv = static_cast<double>(start[f.axes.v] - f.cv);
    const Point end = endOf(arc);
    const double eu = static_cast<double>(end[f.axes.u] - f.cu);
    const double ev = static_cast<double>(end[f.axes.v] - f.cv);
    f.radius = dmath::sqrt(su * su + sv * sv);
    f.a0 = dmath::atan2(sv, su);
    const double a1 = dmath::atan2(ev, eu);
    const bool cw = arc.kind == MoveKind::ArcCW;
    f.direction = cw ? -1.0 : 1.0;
    double sweep = cw ? f.a0 - a1 : a1 - f.a0;
    const bool samePoint = start[f.axes.u] == end[f.axes.u] && start[f.axes.v] == end[f.axes.v];
    if (samePoint) {
        f.full = true;
        sweep = kTwoPi;
    } else {
        while (sweep <= 0.0) {
            sweep += kTwoPi;
        }
        while (sweep > kTwoPi) {
            sweep -= kTwoPi;
        }
    }
    f.sweep = sweep;
    return f;
}

double sweepDegrees(const ArcFrame& f)
{
    return f.full ? 360.0 : f.sweep * (180.0 / kPi);
}

// Point at fraction k/n of the arc: in-plane on the start radius, along the normal by linear interpolation.
Point arcPoint(const ArcFrame& f, const Point& start, const Point& end, std::int64_t k, std::int64_t n)
{
    if (k == n) {
        return end;
    }
    const double angle = f.a0 + f.direction * f.sweep * static_cast<double>(k) / static_cast<double>(n);
    Point p;
    p[f.axes.u] = f.cu + roundToNm(f.radius * dmath::cos(angle));
    p[f.axes.v] = f.cv + roundToNm(f.radius * dmath::sin(angle));
    p[f.axes.w] = lerpNm(start[f.axes.w], end[f.axes.w], k, n);
    return p;
}

class Expander {
public:
    Expander(const cam::Toolpath& path, const lib::MachineDef* machine, const MotionCaps& caps, MotionSink& sink)
        : path_(path), machine_(machine), caps_(caps), sink_(sink)
    {
        pos_ = Point{{path.startX, path.startY, path.startZ}};
    }

    Result<bool> run()
    {
        const auto& moves = path_.moves;
        for (std::size_t index = 0; index < moves.size(); ++index) {
            const Move& m = moves[index];
            const auto moveIndex = static_cast<std::uint32_t>(index);
            const std::optional<CycleKind> cycle = cycleKindOfMove(m.kind);
            if (m.kind == MoveKind::CycleCancel) {
                previousRun_.reset();
                if (runActive_ && !endRun(moveIndex)) {
                    return false;
                }
                continue;
            }
            if (cycle) {
                // A run (cam.h): consecutive cycle moves of one kind, left-hand flag and clearance.
                const RunKey key{*cycle, m.cycleLeftHand, m.cycleClearance};
                const bool runStart = !previousRun_ || !(*previousRun_ == key);
                previousRun_ = key;
                auto ok = checkCycle(m, runStart, moveIndex);
                if (!ok) {
                    return ok;
                }
                if (caps_.cycles[static_cast<std::size_t>(*cycle)]) {
                    auto r = cannedCycle(m, *cycle, runStart, moveIndex);
                    if (!r || !*r) {
                        return r;
                    }
                    continue;
                }
            } else {
                previousRun_.reset();
            }
            if (runActive_ && !endRun(lastCycleMove_)) {
                return false;
            }
            Result<bool> r = true;
            if (cycle) {
                r = expandCycle(m, *cycle, moveIndex);
            } else {
                switch (m.kind) {
                case MoveKind::Rapid:
                    r = emitRapid(endOf(m), moveIndex, true);
                    break;
                case MoveKind::Linear:
                    r = linear(m, moveIndex);
                    break;
                case MoveKind::ArcCW:
                case MoveKind::ArcCCW:
                    r = arc(m, moveIndex);
                    break;
                case MoveKind::Dwell:
                    r = emitDwell(m.cycleDwellMs, moveIndex);
                    break;
                default:
                    break;
                }
            }
            if (!r || !*r) {
                return r;
            }
        }
        if (runActive_ && !endRun(lastCycleMove_)) {
            return false;
        }
        return true;
    }

private:
    bool send(const MotionEvent& e) { return sink_.onEvent(e); }

    Result<bool> checkFeedMove(const Move& m, std::uint32_t index) const
    {
        if (m.feedMicronPerMin <= 0) {
            return tl::unexpected(failure(moveLabel(index) + " is a feed move without a feed"));
        }
        if (m.compensation != 0 && !caps_.cutterComp) {
            return tl::unexpected(failure(moveLabel(index) +
                                          " uses cutter compensation in the control, which the post does not "
                                          "support (capabilities.cutterComp)"));
        }
        return true;
    }

    Result<bool> emitRapid(const Point& to, std::uint32_t index, bool always)
    {
        if (!always && to == pos_) {
            return true;
        }
        MotionEvent e;
        e.kind = EventKind::Rapid;
        e.move = index;
        e.x = to[0];
        e.y = to[1];
        e.z = to[2];
        pos_ = to;
        return send(e);
    }

    Result<bool> emitLinear(const Point& to, std::int32_t feed, MoveRole role, std::uint8_t comp, std::uint32_t index)
    {
        MotionEvent e;
        e.kind = EventKind::Linear;
        e.move = index;
        e.x = to[0];
        e.y = to[1];
        e.z = to[2];
        e.feed = feed;
        e.role = role;
        e.comp = comp;
        pos_ = to;
        return send(e);
    }

    Result<bool> emitDwell(std::int32_t ms, std::uint32_t index)
    {
        MotionEvent e;
        e.kind = EventKind::Dwell;
        e.move = index;
        e.x = pos_[0];
        e.y = pos_[1];
        e.z = pos_[2];
        e.dwellMs = ms;
        return send(e);
    }

    Result<bool> linear(const Move& m, std::uint32_t index)
    {
        auto ok = checkFeedMove(m, index);
        if (!ok) {
            return ok;
        }
        return emitLinear(endOf(m), m.feedMicronPerMin, m.role, m.compensation, index);
    }

    Result<bool> arc(const Move& m, std::uint32_t index)
    {
        auto ok = checkFeedMove(m, index);
        if (!ok) {
            return ok;
        }
        const Point start = pos_;
        const Point end = endOf(m);
        const ArcFrame f = frameOf(m, start);
        if (f.radius < 0.5) { // the start is the centre: no arc; go straight to the end
            return emitLinear(end, m.feedMicronPerMin, m.role, m.compensation, index);
        }
        const bool helical = start[f.axes.w] != end[f.axes.w];
        const auto plane = std::min<std::size_t>(static_cast<std::size_t>(m.plane), 2);
        if (caps_.arcs == ArcOutput::None || !caps_.arcPlanes[plane] ||
            (helical && (!caps_.helical || !caps_.helicalPlanes[plane]))) {
            return linearise(m, f, start, end, index);
        }
        double cap = std::clamp(caps_.maxArcDegrees, 1.0, 360.0);
        if (caps_.arcs == ArcOutput::R) {
            cap = std::min(cap, 180.0);
        }
        const double degrees = sweepDegrees(f);
        auto n = static_cast<std::int64_t>(std::ceil(degrees / cap - 1e-9));
        n = std::max<std::int64_t>(n, 1);
        if (caps_.arcs == ArcOutput::R && degrees / static_cast<double>(n) >= 180.0 - 1e-9) {
            ++n; // R words cannot describe a half circle or more reliably
        }
        const double pieceDegrees = degrees / static_cast<double>(n);
        for (std::int64_t k = 1; k <= n; ++k) {
            const Point pieceStart = pos_;
            const Point to = arcPoint(f, start, end, k, n);
            MotionEvent e;
            e.kind = EventKind::Arc;
            e.move = index;
            e.x = to[0];
            e.y = to[1];
            e.z = to[2];
            e.feed = m.feedMicronPerMin;
            e.role = m.role;
            e.comp = m.compensation;
            e.cw = m.kind == MoveKind::ArcCW;
            e.plane = m.plane;
            Point centre = pieceStart;
            centre[f.axes.u] = f.cu;
            centre[f.axes.v] = f.cv;
            e.cx = centre[0];
            e.cy = centre[1];
            e.cz = centre[2];
            e.i = centre[0] - pieceStart[0];
            e.j = centre[1] - pieceStart[1];
            e.k = centre[2] - pieceStart[2];
            e.radiusNm = f.radius;
            e.sweepDegrees = n == 1 ? degrees : pieceDegrees;
            pos_ = to;
            if (!send(e)) {
                return false;
            }
        }
        return true;
    }

    Result<bool> linearise(const Move& m, const ArcFrame& f, const Point& start, const Point& end, std::uint32_t index)
    {
        std::int64_t n = 1;
        const double tol = static_cast<double>(kLinearizeToleranceNm);
        if (f.radius > tol) {
            const double maxStep = 2.0 * dmath::acos(1.0 - tol / f.radius);
            n = static_cast<std::int64_t>(std::ceil(f.sweep / maxStep - 1e-9));
            n = std::clamp<std::int64_t>(n, 1, kMaxSegmentsPerArc);
        }
        for (std::int64_t k = 1; k <= n; ++k) {
            auto r = emitLinear(arcPoint(f, start, end, k, n), m.feedMicronPerMin, m.role, m.compensation, index);
            if (!r || !*r) {
                return r;
            }
        }
        return true;
    }

    struct RunKey {
        CycleKind kind;
        std::uint8_t leftHand;
        Nm clearance;
        bool operator==(const RunKey&) const = default;
    };

    // What makes a declared cycle's hole `first` when it changes (the return level is a modal word of its own).
    struct CycleParams {
        CycleKind kind;
        Nm bottom, top, retract, clearance, peck, peckClearance;
        std::int32_t dwell, pitch, feed;
        std::uint8_t leftHand;
        bool operator==(const CycleParams&) const = default;
    };

    static CycleParams paramsOf(const Move& m, CycleKind kind)
    {
        return CycleParams{kind,
                           m.z,
                           m.cycleTop,
                           m.cycleRetract,
                           m.cycleClearance,
                           m.cyclePeck,
                           m.cyclePeckClearance,
                           m.cycleDwellMs,
                           m.threadPitchNm,
                           m.feedMicronPerMin,
                           m.cycleLeftHand};
    }

    static Nm returnLevel(const Move& m)
    {
        return m.cycleReturn == cam::CycleReturn::Initial ? m.cycleClearance : m.cycleRetract;
    }

    Result<bool> checkCycle(const Move& m, bool runStart, std::uint32_t index) const
    {
        if (m.feedMicronPerMin <= 0) {
            return tl::unexpected(failure(moveLabel(index) + " is a cycle without a feed"));
        }
        if (!(m.z < m.cycleTop && m.cycleTop <= m.cycleRetract && m.cycleRetract <= m.cycleClearance)) {
            return tl::unexpected(failure(moveLabel(index) +
                                          " is a cycle whose heights are out of order (cam.h needs bottom < top <= "
                                          "R plane <= clearance)"));
        }
        if (runStart && pos_[2] < m.cycleClearance) {
            return tl::unexpected(failure(moveLabel(index) +
                                          " starts a cycle run with the tool below the run's clearance (initial "
                                          "level); cam.h needs it at or above"));
        }
        return true;
    }

    Result<bool> cannedCycle(const Move& m, CycleKind kind, bool runStart, std::uint32_t index)
    {
        const CycleParams params = paramsOf(m, kind);
        if (runActive_ && runStart && !endRun(lastCycleMove_)) {
            return false;
        }
        if (runStart && pos_[2] > m.cycleClearance) {
            // cam.h steps 1 and 2 as plain rapids, so that the control's initial level is the clearance.
            Result<bool> r = emitRapid(Point{{m.x, m.y, pos_[2]}}, index, false);
            if (r && *r) {
                r = emitRapid(Point{{m.x, m.y, m.cycleClearance}}, index, false);
            }
            if (!r || !*r) {
                return r;
            }
        }
        const bool first = !runActive_ || !(params == runParams_);
        runActive_ = true;
        runParams_ = params;
        lastCycleMove_ = index;
        MotionEvent e;
        e.kind = EventKind::Cycle;
        e.move = index;
        e.cycle = kind;
        e.x = m.x;
        e.y = m.y;
        e.z = m.z;
        e.feed = m.feedMicronPerMin;
        e.top = m.cycleTop;
        e.retract = m.cycleRetract;
        e.clearance = m.cycleClearance;
        e.returnLevel =
            m.cycleReturn == cam::CycleReturn::Initial ? cam::CycleReturn::Initial : cam::CycleReturn::RPlane;
        e.peck = m.cyclePeck;
        e.peckClearance = m.cyclePeckClearance;
        e.dwellMs = m.cycleDwellMs;
        e.pitchNm = m.threadPitchNm;
        e.leftHand = m.cycleLeftHand != 0;
        e.first = first;
        pos_ = Point{{m.x, m.y, returnLevel(m)}};
        return send(e);
    }

    bool endRun(std::uint32_t index)
    {
        runActive_ = false;
        MotionEvent e;
        e.kind = EventKind::CycleEnd;
        e.move = index;
        e.x = pos_[0];
        e.y = pos_[1];
        e.z = pos_[2];
        return send(e);
    }

    Result<bool> expandCycle(const Move& m, CycleKind kind, std::uint32_t index)
    {
        if (kind == CycleKind::Tap) {
            return tl::unexpected(failure(moveLabel(index) +
                                          " is a tapping cycle, which the post does not declare "
                                          "(capabilities.cannedCycles); tapping needs spindle reversal and is never "
                                          "expanded into plain moves"));
        }
        const Nm clearance = m.cycleClearance;
        const Nm retract = m.cycleRetract;
        const Nm bottom = m.z;
        const Nm back = cam::peckClearanceNm(m, machine_);
        const std::int32_t feed = m.feedMicronPerMin;
        auto at = [&](Nm z) {
            return Point{{m.x, m.y, z}};
        };
        std::vector<std::pair<bool, Point>> steps; // (rapid?, target); a cycle is a few dozen moves at most
        auto rapid = [&](Nm z) {
            steps.emplace_back(true, at(z));
        };
        auto feedTo = [&](Nm z) {
            steps.emplace_back(false, at(z));
        };

        // cam.h motion steps 1 to 3: over the hole at the tool's Z, down to the clearance if above it, to the R plane.
        rapid(pos_[2]);
        if (pos_[2] > clearance) {
            rapid(clearance);
        }
        rapid(retract);
        const Nm peck = m.cyclePeck;
        const bool stepped = (kind == CycleKind::Peck || kind == CycleKind::ChipBreak) && peck > 0;
        if (!stepped) {
            feedTo(bottom);
        } else {
            Nm depth = retract;
            bool firstStep = true;
            while (depth > bottom) {
                const Nm target = std::max(depth - peck, bottom);
                if (kind == CycleKind::Peck && !firstStep) {
                    rapid(std::min(depth + back, retract)); // G83 re-entry
                }
                feedTo(target);
                if (target > bottom) {
                    if (kind == CycleKind::Peck) {
                        rapid(retract);
                    } else {
                        rapid(std::min(target + back, retract)); // G73 back-off
                    }
                }
                depth = target;
                firstStep = false;
            }
        }
        Result<bool> r = true;
        for (const auto& [isRapid, to] : steps) {
            r = isRapid ? emitRapid(to, index, false) : emitLinear(to, feed, MoveRole::Plunge, 0, index);
            if (!r || !*r) {
                return r;
            }
        }
        if (m.cycleDwellMs > 0) {
            r = emitDwell(m.cycleDwellMs, index);
            if (!r || !*r) {
                return r;
            }
        }
        if (kind == CycleKind::Bore || kind == CycleKind::Ream) {
            r = emitLinear(at(retract), feed, MoveRole::Retract, 0, index); // G85 / G89: feed out
            if (!r || !*r) {
                return r;
            }
        }
        return emitRapid(at(returnLevel(m)), index, false); // G99: R, G98: the clearance
    }

    const cam::Toolpath& path_;
    const lib::MachineDef* machine_;
    const MotionCaps& caps_;
    MotionSink& sink_;
    Point pos_;
    std::optional<RunKey> previousRun_; // the run of the previous move, when it was a cycle move
    bool runActive_ = false;            // a run of declared cycles has had Cycle events and no CycleEnd yet
    CycleParams runParams_{};
    std::uint32_t lastCycleMove_ = 0;
};

} // namespace

std::string_view cycleKindName(CycleKind kind)
{
    switch (kind) {
    case CycleKind::Drill:
        return "drill";
    case CycleKind::Peck:
        return "peck";
    case CycleKind::ChipBreak:
        return "chipBreak";
    case CycleKind::Tap:
        return "tap";
    case CycleKind::Bore:
        return "bore";
    case CycleKind::Ream:
        return "ream";
    }
    return "drill";
}

std::optional<CycleKind> cycleKindFromName(std::string_view name)
{
    for (std::size_t i = 0; i < kCycleKindCount; ++i) {
        const auto kind = static_cast<CycleKind>(i);
        if (cycleKindName(kind) == name) {
            return kind;
        }
    }
    return std::nullopt;
}

std::optional<CycleKind> cycleKindOfMove(cam::MoveKind kind)
{
    switch (kind) {
    case MoveKind::CycleDrill:
        return CycleKind::Drill;
    case MoveKind::CyclePeck:
        return CycleKind::Peck;
    case MoveKind::CycleChipBreak:
        return CycleKind::ChipBreak;
    case MoveKind::CycleTap:
        return CycleKind::Tap;
    case MoveKind::CycleBore:
        return CycleKind::Bore;
    case MoveKind::CycleReam:
        return CycleKind::Ream;
    default:
        return std::nullopt;
    }
}

Result<bool> expandSection(const cam::Toolpath& path, const lib::MachineDef* machine, const MotionCaps& caps,
                           MotionSink& sink)
{
    Expander expander(path, machine, caps, sink);
    return expander.run();
}

ArcGeometry arcGeometry(const cam::Move& arc, Nm startX, Nm startY, Nm startZ)
{
    const ArcFrame f = frameOf(arc, Point{{startX, startY, startZ}});
    return ArcGeometry{f.sweep, f.radius, f.full};
}

SectionStats sectionStats(const cam::Toolpath& path, double rapidMmPerMin)
{
    SectionStats s;
    s.minZ = s.maxZ = path.startZ;
    Point pos{{path.startX, path.startY, path.startZ}};
    auto distance = [](const Point& a, const Point& b) {
        const double dx = static_cast<double>(b[0] - a[0]);
        const double dy = static_cast<double>(b[1] - a[1]);
        const double dz = static_cast<double>(b[2] - a[2]);
        return dmath::sqrt(dx * dx + dy * dy + dz * dz);
    };
    for (const Move& m : path.moves) {
        const Point end = endOf(m);
        s.minZ = std::min(s.minZ, m.z);
        s.maxZ = std::max(s.maxZ, m.z);
        double feedLength = 0;
        switch (m.kind) {
        case MoveKind::Rapid:
            if (rapidMmPerMin > 0) {
                s.seconds += distance(pos, end) / (rapidMmPerMin * 1.0e6) * 60.0;
            }
            pos = end;
            continue;
        case MoveKind::Linear:
            feedLength = distance(pos, end);
            break;
        case MoveKind::ArcCW:
        case MoveKind::ArcCCW: {
            const ArcFrame f = frameOf(m, pos);
            const PlaneAxes a = f.axes;
            const double planar = f.radius * f.sweep;
            const double axial = static_cast<double>(end[a.w] - pos[a.w]);
            feedLength = dmath::sqrt(planar * planar + axial * axial);
            break;
        }
        case MoveKind::Dwell:
            s.seconds += static_cast<double>(m.cycleDwellMs) / 1000.0;
            continue;
        case MoveKind::CycleCancel:
            continue;
        default: // a cycle: in and out between the retract plane and the bottom
            s.maxZ = std::max({s.maxZ, m.cycleRetract, m.cycleClearance});
            feedLength = 2.0 * static_cast<double>(m.cycleRetract - m.z);
            s.seconds += static_cast<double>(m.cycleDwellMs) / 1000.0;
            break;
        }
        s.cutLengthNm += feedLength;
        if (m.feedMicronPerMin > 0) {
            s.seconds += feedLength / (static_cast<double>(m.feedMicronPerMin) * 1000.0) * 60.0;
        }
        if (cycleKindOfMove(m.kind)) {
            // cam.h: the tool ends a cycle at its return level, over the hole.
            pos = Point{{m.x, m.y, m.cycleReturn == cam::CycleReturn::Initial ? m.cycleClearance : m.cycleRetract}};
        } else {
            pos = end;
        }
    }
    return s;
}

} // namespace ray::post
