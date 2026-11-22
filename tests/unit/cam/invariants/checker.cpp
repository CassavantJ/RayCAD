// The toolpath invariant checker (checker.h). README.md in this folder lists the invariants and their error bounds.
#include "checker.h"

#include "toolpath_invariants.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <sstream>

namespace ray::cam::invariants {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kStockEps = 0.001; // a stock contact must be deeper than this (1 um) to count
constexpr double kNmPerMm = 1e6;

std::string num(double v)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

std::string pt(const Vec3& p)
{
    return "(" + num(p.x) + ", " + num(p.y) + ", " + num(p.z) + ")";
}

const char* kindWord(MoveKind k)
{
    switch (k) {
    case MoveKind::Rapid:
        return "rapid";
    case MoveKind::Linear:
        return "linear move";
    case MoveKind::ArcCW:
    case MoveKind::ArcCCW:
        return "arc";
    case MoveKind::Dwell:
        return "dwell";
    case MoveKind::CycleCancel:
        return "cycle cancel";
    default:
        return "cycle";
    }
}

// Findings, one per (move, id): the one with the largest measured value.
class Sink {
public:
    void add(Finding f)
    {
        const auto key = std::make_pair(f.move, f.id);
        const auto it = index_.find(key);
        if (it == index_.end()) {
            index_.emplace(key, list_.size());
            list_.push_back(std::move(f));
        } else if (f.measured > list_[it->second].measured) {
            list_[it->second] = std::move(f);
        }
    }
    bool has(std::size_t move, const std::string& id) const { return index_.count(std::make_pair(move, id)) != 0; }
    std::vector<Finding> take()
    {
        std::stable_sort(list_.begin(), list_.end(), [](const Finding& a, const Finding& b) {
            return a.move < b.move || (a.move == b.move && a.id < b.id);
        });
        return std::move(list_);
    }

private:
    std::vector<Finding> list_;
    std::map<std::pair<std::size_t, std::string>, std::size_t> index_;
};

Vec3 endOf(const Move& m)
{
    return Vec3{mmOf(m.x), mmOf(m.y), mmOf(m.z)};
}

bool isCut(const Piece& p)
{
    return !p.rapid;
}

// ---------- The M2 rules, as findings ----------

void m2Rules(const CheckInput& in, const Motion& mo, Sink& sink)
{
    const Toolpath& path = in.path;
    ToolpathRules rules;
    if (in.claims.spindleRpm > 0) {
        rules.rpm = in.claims.spindleRpm;
    }
    rules.stock = in.geom.stockBounds;
    const double floor = in.claims.rapidFloorZ ? *in.claims.rapidFloorZ : in.geom.stockBounds.max.z;
    rules.rapidFloorZ = toNm(floor);
    if (in.claims.floorZ) {
        rules.floorZ = toNm(*in.claims.floorZ);
    }
    if (in.machine != nullptr) {
        rules.axes = in.machine->axes;
    }
    rules.wcsInMachine = in.claims.wcsInMachine;
    for (const Violation& v : checkToolpath(path, rules)) {
        const Move& m = path.moves[v.move];
        if (v.rule == "rapidLow" && m.kind == MoveKind::Rapid) {
            // A retract straight up (same X and Y, Z rising) leaves along the tool's own axis through space the tool
            // already holds: the M2 rule, written for drilling where no plain rapid starts inside the stock, counts it;
            // milling retracts from the cut that way. Its contacts are still checked (rapidPart, rapidStock).
            const Vec3 from = mo.startOf[v.move];
            if (toNm(from.x) == m.x && toNm(from.y) == m.y && m.z > toNm(from.z)) {
                continue;
            }
        }
        Finding f;
        f.id = v.rule;
        f.move = v.move;
        f.at = endOf(m);
        const Vec3 from = mo.startOf[v.move];
        if (v.rule == "rapidLow") {
            f.measured = std::min(from.z, isCycle(m.kind) ? from.z : f.at.z);
            f.allowed = floor;
            f.message = "a rapid over the stock goes below the rapid floor Z " + num(floor);
        } else if (v.rule == "belowFloor") {
            f.measured = f.at.z;
            f.allowed = *in.claims.floorZ;
            f.message = "the tool goes below the floor Z " + num(*in.claims.floorZ);
        } else if (v.rule == "travel") {
            f.message = "a point outside the machine's travel";
        } else if (v.rule == "rapidFeed") {
            f.measured = m.feedMicronPerMin;
            f.message = "a rapid carries a feed";
        } else if (v.rule == "feedMove" || v.rule == "feed") {
            f.measured = m.feedMicronPerMin;
            f.allowed = 1;
            f.message = std::string("a ") + kindWord(m.kind) + " without a positive feed";
        } else if (v.rule == "tapFeed") {
            f.measured = m.feedMicronPerMin;
            f.message = "the tap feed is not pitch times rpm";
        } else {
            f.message = "canned cycle rule '" + v.rule + "' (cycle_invariants.h) broken";
        }
        sink.add(std::move(f));
    }
}

// ---------- Rules on the moves themselves ----------

void moveRules(const CheckInput& in, const Motion& mo, Sink& sink)
{
    const Toolpath& path = in.path;
    const Claims& c = in.claims;
    const std::size_t n = path.moves.size();
    // enum
    for (std::size_t k = 0; k < n; ++k) {
        const Move& m = path.moves[k];
        const bool bad = static_cast<int>(m.kind) > static_cast<int>(MoveKind::CycleCancel) ||
                         static_cast<int>(m.role) > static_cast<int>(MoveRole::Approach) ||
                         static_cast<int>(m.plane) > static_cast<int>(ArcPlane::YZ) || m.compensation > 2 ||
                         static_cast<int>(m.cycleReturn) > static_cast<int>(CycleReturn::Initial);
        if (bad) {
            sink.add(Finding{"enum", k, endOf(m), 0, 0, "a field holds a value outside its enumeration"});
        }
    }
    // spindle: cutting needs the spindle on, within the machine's range, turning the way the tool cuts.
    std::size_t firstCut = n;
    bool taps = false;
    for (const Piece& p : mo.pieces) {
        if (isCut(p)) {
            firstCut = std::min(firstCut, p.move);
        }
    }
    for (const Move& m : path.moves) {
        taps = taps || m.kind == MoveKind::CycleTap;
    }
    if (firstCut < n) {
        const Vec3 at = endOf(path.moves[firstCut]);
        if (!(c.spindleRpm > 0)) {
            sink.add(Finding{"spindle", firstCut, at, c.spindleRpm, 0, "cutting with the spindle stopped"});
        } else if (in.machine != nullptr && in.machine->maxRpm > 0 && c.spindleRpm > in.machine->maxRpm) {
            sink.add(Finding{"spindle", firstCut, at, c.spindleRpm, in.machine->maxRpm,
                             "the spindle speed is above the machine's maximum"});
        } else if (in.machine != nullptr && in.machine->minRpm > 0 && c.spindleRpm < in.machine->minRpm) {
            sink.add(Finding{"spindle", firstCut, at, c.spindleRpm, in.machine->minRpm,
                             "the spindle speed is below the machine's minimum"});
        } else if (!c.clockwise && !taps) {
            sink.add(Finding{"spindle", firstCut, at, 0, 1,
                             "the spindle turns counter-clockwise; the tool cuts clockwise (right hand)"});
        }
    }
    // arc, compensation, continuity: on integer positions.
    Nm x = path.startX, y = path.startY, z = path.startZ;
    int comp = 0;
    std::size_t lastMotion = n;
    for (std::size_t k = 0; k < n; ++k) {
        const Move& m = path.moves[k];
        const Vec3 at = endOf(m);
        const bool cycle = isCycle(m.kind);
        if (m.kind == MoveKind::Dwell || m.kind == MoveKind::CycleCancel) {
            if (m.x != x || m.y != y || m.z != z) {
                sink.add(Finding{"continuity", k, at,
                                 std::hypot(std::hypot(mmOf(m.x - x), mmOf(m.y - y)), mmOf(m.z - z)), 0,
                                 std::string("a ") + kindWord(m.kind) +
                                     " does not carry the tool's position (the tool does not move there)"});
            }
            continue;
        }
        lastMotion = k;
        if (m.kind == MoveKind::ArcCW || m.kind == MoveKind::ArcCCW) {
            // (u, v) of the plane, as motion.h.
            const auto uv = [&](Nm px, Nm py, Nm pz) {
                switch (m.plane) {
                case ArcPlane::XZ:
                    return std::make_pair(pz, px);
                case ArcPlane::YZ:
                    return std::make_pair(py, pz);
                default:
                    return std::make_pair(px, py);
                }
            };
            const auto s = uv(x, y, z), e = uv(m.x, m.y, m.z), ce = uv(m.i, m.j, m.k);
            const double rs =
                std::hypot(static_cast<double>(s.first - ce.first), static_cast<double>(s.second - ce.second));
            const double re =
                std::hypot(static_cast<double>(e.first - ce.first), static_cast<double>(e.second - ce.second));
            const double chord =
                std::hypot(static_cast<double>(e.first - s.first), static_cast<double>(e.second - s.second));
            if (std::abs(rs - re) > static_cast<double>(c.arcRadiusTolNm)) {
                sink.add(Finding{"arc", k, at, std::abs(rs - re), static_cast<double>(c.arcRadiusTolNm),
                                 "the arc's start and end radius differ (nm)"});
            } else if (rs < 1000) {
                sink.add(Finding{"arc", k, at, rs, 1000, "the arc's radius is below 1 um (nm)"});
            } else if (chord > 0 && chord < 1000) {
                sink.add(Finding{"arc", k, at, chord, 1000,
                                 "the arc's start and end are closer than 1 um but not equal: a full turn or "
                                 "nothing (nm)"});
            }
        }
        if (m.compensation <= 2) {
            if (m.compensation != comp && m.kind != MoveKind::Linear) {
                sink.add(Finding{"compensation", k, at, static_cast<double>(m.compensation), static_cast<double>(comp),
                                 std::string("cutter compensation turned on or off on a ") + kindWord(m.kind) +
                                     " (only a linear move may)"});
            }
            if (cycle && m.compensation != 0) {
                sink.add(Finding{"compensation", k, at, static_cast<double>(m.compensation), 0,
                                 "a canned cycle with cutter compensation on"});
            }
            if (m.compensation != 0 && c.direction != Direction::Any) {
                // Climb with a clockwise spindle keeps the material on the right: the tool left of it (G41).
                const bool climb = c.direction == Direction::Climb;
                const int want = (climb == c.clockwise) ? 1 : 2;
                if (m.compensation != want) {
                    sink.add(Finding{"compensation", k, at, static_cast<double>(m.compensation),
                                     static_cast<double>(want),
                                     std::string("compensation ") + (m.compensation == 1 ? "left" : "right") +
                                         " cuts " + (climb ? "conventional" : "climb") + "; " +
                                         (climb ? "climb" : "conventional") + " was asked for"});
                }
            }
            comp = m.compensation;
        }
        x = m.x;
        y = m.y;
        z = cycle ? (m.cycleReturn == CycleReturn::Initial ? m.cycleClearance : m.cycleRetract) : m.z;
    }
    if (comp != 0 && lastMotion < n) {
        sink.add(Finding{"compensation", lastMotion, endOf(path.moves[lastMotion]), static_cast<double>(comp), 0,
                         "cutter compensation is still on at the end of the toolpath"});
    }
    // leadTangent: the last lead-in move meets the cut, and the cut meets the first lead-out move, tangentially in XY.
    std::vector<int> firstPiece(n, -1), lastPiece(n, -1);
    for (std::size_t k = 0; k < mo.pieces.size(); ++k) {
        const std::size_t mv = mo.pieces[k].move;
        if (firstPiece[mv] < 0) {
            firstPiece[mv] = static_cast<int>(k);
        }
        lastPiece[mv] = static_cast<int>(k);
    }
    std::size_t prev = n;
    for (std::size_t k = 0; k < n; ++k) {
        if (firstPiece[k] < 0) {
            continue;
        }
        if (prev < n) {
            const MoveRole a = path.moves[prev].role, b = path.moves[k].role;
            const bool joint =
                (a == MoveRole::LeadIn && b == MoveRole::Cut) || (a == MoveRole::Cut && b == MoveRole::LeadOut);
            const Piece& pa = mo.pieces[static_cast<std::size_t>(lastPiece[prev])];
            const Piece& pb = mo.pieces[static_cast<std::size_t>(firstPiece[k])];
            if (joint && !pa.rapid && !pb.rapid && pa.xyLength() > 1e-6 && pb.xyLength() > 1e-6) {
                const Vec2 da = pa.direction(1), db = pb.direction(0);
                const double ang = std::atan2(da.x * db.y - da.y * db.x, da.x * db.x + da.y * db.y) * 180 / kPi;
                if (std::abs(ang) > c.leadTangentTolDeg) {
                    sink.add(Finding{"leadTangent", k, pb.a, std::abs(ang), c.leadTangentTolDeg,
                                     std::string(a == MoveRole::LeadIn ? "the lead-in meets the cut"
                                                                       : "the lead-out leaves the cut") +
                                         " at an angle (degrees)"});
                }
            }
        }
        prev = k;
    }
    // heights: the claimed levels in order, and the toolpath starting and ending at the clearance.
    const AbsHeights& h = c.heights;
    const std::vector<std::pair<const char*, std::optional<double>>> levels = {{"clearance", h.clearance},
                                                                               {"retract", h.retract},
                                                                               {"feed plane", h.feedPlane},
                                                                               {"top", h.top},
                                                                               {"bottom", h.bottom}};
    for (std::size_t a = 0; a < levels.size(); ++a) {
        for (std::size_t b = a + 1; b < levels.size(); ++b) {
            if (levels[a].second && levels[b].second && *levels[a].second < *levels[b].second) {
                sink.add(Finding{"heights", n, Vec3{}, *levels[a].second, *levels[b].second,
                                 std::string("the ") + levels[a].first + " is below the " + levels[b].first});
            }
        }
    }
    if (h.clearance && n > 0) {
        const double eps = 1e-6;
        if (mmOf(path.startZ) < *h.clearance - eps) {
            sink.add(Finding{"heights", 0, mo.startOf[0], mmOf(path.startZ), *h.clearance,
                             "the toolpath starts below the clearance height"});
        }
        if (mo.startOf.back().z < *h.clearance - eps) {
            sink.add(Finding{"heights", lastMotion < n ? lastMotion : n - 1, mo.startOf.back(), mo.startOf.back().z,
                             *h.clearance, "the toolpath ends below the clearance height"});
        }
    }
    // containment: the tool's centre on feed moves inside the region grown by the tolerance.
    if (c.containment) {
        const ContainClaim& cc = *c.containment;
        const double step = std::min(std::max(cc.tolerance, 1e-4), 0.05);
        for (const Piece& p : mo.pieces) {
            if (p.rapid || sink.has(p.move, "containment")) {
                continue;
            }
            const int count = std::max(1, static_cast<int>(std::ceil(p.xyLength() / step)));
            double worst = 0;
            Vec3 at{};
            for (int k = 0; k <= count; ++k) {
                const Vec3 q = p.at(static_cast<double>(k) / count);
                if (cc.region.contains(q.x, q.y, in.geom.stockBounds)) {
                    continue;
                }
                const double out = cc.region.boundaryDistance(q.x, q.y, in.geom.stockBounds);
                if (out > worst) {
                    worst = out;
                    at = q;
                }
            }
            if (worst > cc.tolerance) {
                sink.add(Finding{"containment", p.move, at, worst, cc.tolerance,
                                 "the tool's centre leaves the containment region"});
            }
        }
    }
    // travel, the bulge of arcs (the M2 rule checks end points).
    if (in.machine != nullptr && !in.machine->axes.empty()) {
        for (const Piece& p : mo.pieces) {
            if (!p.arc) {
                continue;
            }
            for (int q = 0; q < 4; ++q) {
                const double ang = q * kPi / 2;
                double s = p.sweep > 0 ? std::fmod(ang - p.th0, 2 * kPi) : std::fmod(p.th0 - ang, 2 * kPi);
                if (s < 0) {
                    s += 2 * kPi;
                }
                if (s > std::abs(p.sweep)) {
                    continue;
                }
                const Vec3 e{p.cx + p.ra * std::cos(ang), p.cy + p.ra * std::sin(ang), p.a.z};
                for (const lib::AxisDef& ax : in.machine->axes) {
                    if (!(ax.maxTravel > ax.minTravel) || (ax.letter != 'X' && ax.letter != 'Y')) {
                        continue;
                    }
                    const double v = ax.letter == 'X' ? e.x + c.wcsInMachine.x : e.y + c.wcsInMachine.y;
                    if (v < ax.minTravel - 1e-9 || v > ax.maxTravel + 1e-9) {
                        sink.add(Finding{"travel", p.move, e, v, v < ax.minTravel ? ax.minTravel : ax.maxTravel,
                                         std::string("the arc bulges outside the machine's ") + ax.letter + " travel"});
                    }
                }
            }
        }
    }
}

// ---------- Contacts with the part (conservative, refined) ----------

struct PartCheck {
    const PartModel& pm;
    const ToolProfile& prof;
    std::span<const AllowedCut> allowed;
    double minCell;

    struct Result {
        bool found = false;
        double measured = 0;
        Vec3 at{};
    };

    // The largest penetration of the protected material (grown by w in XY, f in Z) into `zone` along piece p beyond
    // `limit`, if any.
    Result run(const Piece& p, Zone zone, double w, double f, double limit) const
    {
        Result res;
        if (pm.empty() || !prof.hasZone(zone)) {
            return res;
        }
        const double rz = prof.maxRadius(zone) + w;
        const double zb = prof.zoneBottom(zone);
        const double h = pm.cell();
        double bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
        p.box(bx0, by0, bx1, by1);
        const double grow = rz + h + p.inflate;
        const int i0 = std::max(0, static_cast<int>(std::floor((bx0 - grow - pm.x0()) / h)));
        const int i1 = std::min(pm.nx() - 1, static_cast<int>(std::floor((bx1 + grow - pm.x0()) / h)));
        const int j0 = std::max(0, static_cast<int>(std::floor((by0 - grow - pm.y0()) / h)));
        const int j1 = std::min(pm.ny() - 1, static_cast<int>(std::floor((by1 + grow - pm.y0()) / h)));
        if (i0 > i1 || j0 > j1) {
            return res;
        }
        const double floorZ = p.zmin() + zb - f - 1e-9;
        std::vector<Iv> zs;
        for (int bj = j0 / PartModel::kBlock; bj <= j1 / PartModel::kBlock; ++bj) {
            for (int bi = i0 / PartModel::kBlock; bi <= i1 / PartModel::kBlock; ++bi) {
                if (pm.blockTop(bi, bj) < floorZ) {
                    continue;
                }
                for (int j = std::max(j0, bj * PartModel::kBlock); j <= std::min(j1, bj * PartModel::kBlock + 7); ++j) {
                    for (int i = std::max(i0, bi * PartModel::kBlock); i <= std::min(i1, bi * PartModel::kBlock + 7);
                         ++i) {
                        square(p, zone, w, f, limit, rz, i, j, pm.centreX(i), pm.centreY(j), h / 2, zs, res);
                    }
                }
            }
        }
        return res;
    }

    void square(const Piece& p, Zone zone, double w, double f, double limit, double rz, int i, int j, double qx,
                double qy, double s, std::vector<Iv>& zs, Result& res) const
    {
        const double hd = s * std::sqrt(2.0);
        const Piece::Near n = p.nearest(qx, qy);
        if (n.d - hd - p.inflate > rz) {
            return;
        }
        pm.zset(i, j, qx, qy, s, allowed, zs);
        if (zs.empty()) {
            return;
        }
        const auto zr = p.zRange(qx, qy, rz + hd + p.inflate, n);
        const double dUp = n.d - hd - p.inflate - w;
        const double dLo = n.d + hd + p.inflate - w;
        PenBounds best;
        for (const Iv& iv : zs) {
            const PenBounds b =
                penetration(prof, zone, iv.lo - f, iv.hi + f, zr.first, zr.second, n.z + p.inflate, dUp, dLo);
            if (b.up > best.up) {
                best.up = b.up;
                best.zUp = b.zUp;
            }
            best.lo = std::max(best.lo, b.lo);
        }
        if (best.up <= limit) {
            return; // no contact beyond the limit anywhere over this square
        }
        if (res.found && best.up <= res.measured) {
            return; // cannot change the verdict or the worst value already found
        }
        if (best.lo > limit) {
            // A real contact. Refined further only to measure it (until the bounds are within a tenth of it).
            record(res, best.lo, Vec3{qx, qy, best.zUp});
            if (best.up - best.lo <= 0.1 * best.lo || s * 2 <= minCell) {
                return;
            }
        } else if (s * 2 <= minCell) {
            // Undecided at the finest square: reported, on the side of safety.
            record(res, best.up, Vec3{qx, qy, best.zUp});
            return;
        }
        const double c = s / 2;
        std::vector<Iv> sub;
        for (const auto& [dx, dy] : {std::pair{-c, -c}, std::pair{c, -c}, std::pair{-c, c}, std::pair{c, c}}) {
            square(p, zone, w, f, limit, rz, i, j, qx + dx, qy + dy, c, sub, res);
        }
    }

    static void record(Result& r, double v, Vec3 at)
    {
        if (!r.found || v > r.measured) {
            r.found = true;
            r.measured = v;
            r.at = at;
        }
    }
};

// ---------- Euclidean distance transform (Felzenszwalb and Huttenlocher), in samples ----------

void edt1(const double* f, double* d, int n, std::vector<int>& v, std::vector<double>& zz)
{
    v.assign(static_cast<std::size_t>(n), 0);
    zz.assign(static_cast<std::size_t>(n) + 1, 0);
    int k = 0;
    v[0] = 0;
    zz[0] = -std::numeric_limits<double>::infinity();
    zz[1] = std::numeric_limits<double>::infinity();
    for (int q = 1; q < n; ++q) {
        if (std::isinf(f[q])) {
            continue;
        }
        if (std::isinf(f[v[static_cast<std::size_t>(k)]])) {
            v[static_cast<std::size_t>(k)] = q;
            continue;
        }
        double s = 0;
        while (true) {
            const int r = v[static_cast<std::size_t>(k)];
            s = ((f[q] + q * static_cast<double>(q)) - (f[r] + r * static_cast<double>(r))) / (2.0 * q - 2.0 * r);
            if (s <= zz[static_cast<std::size_t>(k)] && k > 0) {
                --k;
                continue;
            }
            break;
        }
        ++k;
        v[static_cast<std::size_t>(k)] = q;
        zz[static_cast<std::size_t>(k)] = s;
        zz[static_cast<std::size_t>(k) + 1] = std::numeric_limits<double>::infinity();
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (zz[static_cast<std::size_t>(k) + 1] < q) {
            ++k;
        }
        const int r = v[static_cast<std::size_t>(k)];
        d[q] = std::isinf(f[r]) ? std::numeric_limits<double>::infinity() : (q - r) * static_cast<double>(q - r) + f[r];
    }
}

// Distance (in samples) from each sample to the nearest set one; +inf when none is set.
std::vector<double> distanceTo(const std::vector<std::uint8_t>& set, int nx, int ny)
{
    const double inf = std::numeric_limits<double>::infinity();
    std::vector<double> g(set.size());
    for (std::size_t k = 0; k < set.size(); ++k) {
        g[k] = set[k] ? 0 : inf;
    }
    std::vector<int> v;
    std::vector<double> zz, f(static_cast<std::size_t>(std::max(nx, ny))), d(f.size());
    for (int i = 0; i < nx; ++i) {
        for (int j = 0; j < ny; ++j) {
            f[static_cast<std::size_t>(j)] = g[static_cast<std::size_t>(j) * nx + i];
        }
        edt1(f.data(), d.data(), ny, v, zz);
        for (int j = 0; j < ny; ++j) {
            g[static_cast<std::size_t>(j) * nx + i] = d[static_cast<std::size_t>(j)];
        }
    }
    for (int j = 0; j < ny; ++j) {
        double* row = g.data() + static_cast<std::size_t>(j) * nx;
        std::copy(row, row + nx, f.begin());
        edt1(f.data(), d.data(), nx, v, zz);
        for (int i = 0; i < nx; ++i) {
            row[i] = std::sqrt(d[static_cast<std::size_t>(i)]);
        }
    }
    return g;
}

// ---------- The geometric pass ----------

class Geometry {
public:
    Geometry(const CheckInput& in, const CheckOptions& o, const PartModel& pm, Sink& sink)
        : in_(in), c_(in.claims), o_(o), pm_(pm), sink_(sink),
          prof_(makeProfile(in.tool, in.holder, in.claims.stickOut)),
          partCheck_{pm, prof_, std::span<const AllowedCut>(in.claims.allowedCuts), o.minCell}
    {
        rCut_ = prof_.cuttingRadius();
        const Box3& s = in.geom.stockBounds;
        const double longer = std::max(s.max.x - s.min.x, s.max.y - s.min.y);
        const double hs = o.stockCell > 0 ? o.stockCell : std::max(0.1, longer / 1500);
        stock_.init(s, hs, rCut_ + 4 * hs + 1, c_.initialStock, &pm, c_.allowedCuts);
        centreCutting_ = c_.centreCutting
                             ? *c_.centreCutting
                             : centreCuttingByDefault(in.tool.type) &&
                                   !(in.tool.type == lib::ToolType::ChamferMill && in.tool.tipDiameter > 0);
        lf_ = prof_.zoneBottom(Zone::Body);
        if (std::isinf(lf_)) {
            lf_ = prof_.zoneBottom(Zone::Holder);
        }
    }

    template <class Lap>
    void run(const Motion& mo, Lap&& lap)
    {
        for (const Piece& p : mo.pieces) {
            partContacts(p);
            if (!p.rapid) {
                entry(p);
                if (c_.direction != Direction::Any && p.role == MoveRole::Cut && p.xyLength() > 1e-6) {
                    direction(p);
                }
            }
            if (c_.maxEngagementDeg && !p.rapid && p.role == MoveRole::Cut && p.xyLength() > 1e-6) {
                engagementAndStock(p);
            } else {
                stockPass(p);
            }
        }
        lap("moves against part and stock");
        for (const ClearClaim& cl : c_.clears) {
            clear(cl);
        }
        for (const WallClaim& wc : c_.walls) {
            walls(wc);
        }
        lap("region claims");
    }

private:
    void add(const Piece& p, const char* id, double measured, double allowed, Vec3 at, std::string msg)
    {
        sink_.add(Finding{id, p.move, at, measured, allowed, std::move(msg)});
    }

    void partContacts(const Piece& p)
    {
        if (pm_.empty()) {
            return;
        }
        if (p.rapid) {
            for (const Zone z : {Zone::Cutting, Zone::Body, Zone::Holder}) {
                if (sink_.has(p.move, "rapidPart")) {
                    break;
                }
                const auto r = partCheck_.run(p, z, c_.rapidClearance, c_.rapidClearance, c_.tolerance);
                if (r.found) {
                    add(p, "rapidPart", r.measured, c_.tolerance, r.at,
                        std::string("a rapid runs the ") + zoneName(z) + " " + num(r.measured) +
                            " mm into the part (clearance " + num(c_.rapidClearance) + " mm)");
                }
            }
            return;
        }
        if (!sink_.has(p.move, "gouge")) {
            const auto r = partCheck_.run(p, Zone::Cutting, c_.stockToLeaveWall, c_.stockToLeaveFloor, c_.tolerance);
            if (r.found) {
                add(p, "gouge", r.measured, c_.tolerance, r.at,
                    "the flutes cut " + num(r.measured) + " mm into the part (stock to leave " +
                        num(c_.stockToLeaveWall) + " wall, " + num(c_.stockToLeaveFloor) + " floor)");
            }
        }
        if (!sink_.has(p.move, "shank")) {
            const auto r = partCheck_.run(p, Zone::Body, 0, 0, c_.tolerance);
            if (r.found) {
                add(p, "shank", r.measured, c_.tolerance, r.at,
                    "the tool above its flutes runs " + num(r.measured) + " mm into the part");
            }
        }
        if (!sink_.has(p.move, "holder")) {
            const auto r = partCheck_.run(p, Zone::Holder, c_.holderClearance, c_.holderClearance, c_.tolerance);
            if (r.found) {
                add(p, "holder", r.measured, c_.tolerance, r.at,
                    "the holder comes " + num(r.measured) + " mm inside its clearance of " + num(c_.holderClearance) +
                        " mm to the part");
            }
        }
    }

    // Samples of the stock grid within `radius` of the piece's path: f(index, x, y, near). Samples whose stock lies
    // wholly below `belowZ` are skipped, and so are those farther than rNear whose stock lies below `farBelowZ` (the
    // holder's ring reaches only stock above the holder's nose).
    template <class F>
    void samples(const Piece& p, double radius, double belowZ, F&& f, double rNear = kInf, double farBelowZ = 0)
    {
        double bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
        p.box(bx0, by0, bx1, by1);
        const int i0 = stock_.col(bx0 - radius), i1 = std::min(stock_.nx() - 1, stock_.col(bx1 + radius));
        const int j0 = stock_.row(by0 - radius), j1 = std::min(stock_.ny() - 1, stock_.row(by1 + radius));
        const double hs = stock_.cell();
        const double halfBlock = StockGrid::kBlock * hs * std::sqrt(2.0) / 2;
        for (int bj = j0 / StockGrid::kBlock; bj <= j1 / StockGrid::kBlock; ++bj) {
            for (int bi = i0 / StockGrid::kBlock; bi <= i1 / StockGrid::kBlock; ++bi) {
                const double top = stock_.blockTop(bi, bj);
                if (top < belowZ) {
                    continue;
                }
                const double bcx = stock_.x(bi * StockGrid::kBlock) + (StockGrid::kBlock - 1) * hs / 2;
                const double bcy = stock_.y(bj * StockGrid::kBlock) + (StockGrid::kBlock - 1) * hs / 2;
                const double bd = p.nearest(bcx, bcy).d - halfBlock;
                if (bd > radius || (bd > rNear && top < farBelowZ)) {
                    continue;
                }
                for (int j = std::max(j0, bj * StockGrid::kBlock); j <= std::min(j1, bj * StockGrid::kBlock + 7); ++j) {
                    for (int i = std::max(i0, bi * StockGrid::kBlock); i <= std::min(i1, bi * StockGrid::kBlock + 7);
                         ++i) {
                        const int idx = stock_.index(i, j);
                        if (stock_.at(idx).empty()) {
                            continue;
                        }
                        const double x = stock_.x(i), y = stock_.y(j);
                        const Piece::Near n = p.nearest(x, y);
                        if (n.d > radius || (n.d > rNear && stock_.top(idx) < farBelowZ)) {
                            continue;
                        }
                        f(idx, x, y, n);
                    }
                }
            }
        }
    }

    // Plunges, ramps and helices into stock (before the piece removes anything).
    void entry(const Piece& p)
    {
        if (!(p.a.z > p.b.z)) {
            return;
        }
        const bool straight = p.xyLength() < 1e-3;
        if (straight) {
            if (centreCutting_) {
                return;
            }
            const double rDead = c_.deadZoneFraction * rCut_;
            double depth = 0;
            Vec3 at{};
            samples(p, rDead, p.zmin(), [&](int idx, double x, double y, const Piece::Near&) {
                for (const StockGrid::SIv& iv : stock_.at(idx)) {
                    const double d = std::min<double>(iv.hi, p.a.z) - std::max<double>(iv.lo, p.b.z);
                    if (d > depth) {
                        depth = d;
                        at = Vec3{x, y, std::min<double>(iv.hi, p.a.z)};
                    }
                }
            });
            if (depth > kStockEps) {
                add(p, "plunge", depth, 0, at,
                    "a straight plunge " + num(depth) + " mm into stock with a tool that does not cut at its centre");
            }
            return;
        }
        const double angle = std::atan2(p.a.z - p.b.z, p.xyLength()) * 180 / kPi;
        const bool steep = c_.maxRampAngleDeg && angle > *c_.maxRampAngleDeg + 1e-6;
        const bool narrow = p.arc && c_.helixMinDiameter && 2 * p.ra < *c_.helixMinDiameter - 1e-9;
        if (!steep && !narrow) {
            return;
        }
        bool engaged = false;
        Vec3 at{};
        samples(p, rCut_, p.zmin(), [&](int idx, double x, double y, const Piece::Near& n) {
            for (const StockGrid::SIv& iv : stock_.at(idx)) {
                if (!engaged && iv.hi > n.z + kStockEps && iv.lo < n.z + lf_) {
                    engaged = true;
                    at = Vec3{x, y, n.z};
                }
            }
        });
        if (!engaged) {
            return;
        }
        if (steep) {
            add(p, "rampAngle", angle, *c_.maxRampAngleDeg, at,
                std::string(p.arc ? "a helix" : "a ramp") + " into stock descends at " + num(angle) + " degrees");
        }
        if (narrow) {
            add(p, "helix", 2 * p.ra, *c_.helixMinDiameter, at,
                "a helix into stock of diameter " + num(2 * p.ra) + " mm, below the minimum");
        }
    }

    // Contacts of the shank and holder (cuts) or of the whole tool (rapids) with the stock, and the stock the piece
    // removes.
    void stockPass(const Piece& whole)
    {
        const bool sloped = !whole.vertical() && whole.a.z != whole.b.z;
        const int parts =
            sloped ? std::max(1, static_cast<int>(std::ceil((whole.zmax() - whole.zmin()) / o_.rampStepMm))) : 1;
        for (int k = 0; k < parts; ++k) {
            const Piece p =
                parts == 1 ? whole : whole.part(static_cast<double>(k) / parts, static_cast<double>(k + 1) / parts);
            stockPiece(p);
        }
    }

    void stockPiece(const Piece& p)
    {
        const double w = p.rapid ? c_.rapidClearance : c_.holderClearance;
        const double rAll = prof_.maxRadius() + w;
        const bool vertical = p.vertical();
        const double zMin = p.zmin(), zMax = p.zmax();
        double worstBody = 0, worstHolder = 0, worstRapid = 0;
        Vec3 atBody{}, atHolder{}, atRapid{};
        Zone rapidZone = Zone::Cutting;
        const double rNear = std::max(prof_.maxRadius(Zone::Cutting), prof_.maxRadius(Zone::Body)) + w;
        const double farBelow = prof_.hasZone(Zone::Holder) ? zMin + prof_.zoneBottom(Zone::Holder) - w : zMin - w;
        samples(
            p, rAll, zMin - w,
            [&](int idx, double x, double y, const Piece::Near& n) {
                const double zt = vertical ? zMin : n.z;
                for (const StockGrid::SIv& iv : stock_.at(idx)) {
                    if (p.rapid) {
                        for (const Zone z : {Zone::Cutting, Zone::Body, Zone::Holder}) {
                            const PenBounds b =
                                penetration(prof_, z, iv.lo - w, iv.hi + w, zt, zt, zt, n.d - w, n.d - w);
                            if (b.lo > worstRapid) {
                                worstRapid = b.lo;
                                atRapid = Vec3{x, y, b.zUp};
                                rapidZone = z;
                            }
                        }
                        continue;
                    }
                    const PenBounds body = penetration(prof_, Zone::Body, iv.lo, iv.hi, zt, zt, zt, n.d, n.d);
                    if (body.lo > worstBody) {
                        worstBody = body.lo;
                        atBody = Vec3{x, y, body.zUp};
                    }
                    const PenBounds hold =
                        penetration(prof_, Zone::Holder, iv.lo - c_.holderClearance, iv.hi + c_.holderClearance, zt, zt,
                                    zt, n.d - c_.holderClearance, n.d - c_.holderClearance);
                    if (hold.lo > worstHolder) {
                        worstHolder = hold.lo;
                        atHolder = Vec3{x, y, hold.zUp};
                    }
                }
                if (vertical) {
                    // Every tip height at one place: the union of the runs over them, exactly.
                    prof_.occupied(n.d, runs_);
                    for (const auto& [a, b] : runs_) {
                        stock_.subtract(idx, a + zMin, b + zMax);
                    }
                    return;
                }
                // Sloped or level: what the nearest position and the two ends of the reach of the tip's flat (or of the
                // flutes) remove, each a real position, so stock is never removed that the tool did not cut.
                const double flat = prof_.radiusAt(0);
                const auto [ta, tb] = p.reach(x, y, flat > 0 ? flat : rNear, n);
                for (const double t : {n.t, ta, tb}) {
                    const Vec3 pos = p.at(t);
                    const double d = t == n.t ? n.d : std::hypot(x - pos.x, y - pos.y);
                    const double tip = t == n.t ? n.z : pos.z;
                    prof_.occupied(d, runs_);
                    for (const auto& [a, b] : runs_) {
                        stock_.subtract(idx, a + tip, b + tip);
                    }
                    if (zMin == zMax) {
                        break; // level: the nearest position removes the most
                    }
                }
            },
            rNear, farBelow);
        if (worstRapid > kStockEps) {
            add(p, "rapidStock", worstRapid, 0, atRapid,
                std::string("a rapid runs the ") + zoneName(rapidZone) + " " + num(worstRapid) + " mm into stock");
        }
        if (worstBody > kStockEps) {
            add(p, "fluteLength", worstBody, 0, atBody,
                "stock reaches " + num(worstBody) +
                    " mm into the tool above its flutes (cut deeper than the flute length)");
        }
        if (worstHolder > kStockEps) {
            add(p, "holder", worstHolder, 0, atHolder,
                "the holder comes " + num(worstHolder) + " mm inside its clearance of " + num(c_.holderClearance) +
                    " mm to the stock");
        }
    }

    // Engagement: the arc of the cutter's circumference in contact with stock, stepping along the piece. Only the front
    // half of the circle can meet stock that earlier positions left (the rear half lies inside them); each of its
    // points, 1 degree apart, reads the stock at the nearest sample, at the tool's cutting heights.
    void engagementAndStock(const Piece& whole)
    {
        const double hs = stock_.cell();
        const int steps = std::max(1, static_cast<int>(std::ceil(whole.xyLength() / hs)));
        constexpr int kBins = 180;
        double worst = 0;
        Vec3 at{};
        for (int k = 1; k <= steps; ++k) {
            const double t0 = static_cast<double>(k - 1) / steps, t1 = static_cast<double>(k) / steps;
            const Vec3 pos = whole.at(t1);
            const Vec2 dir = whole.direction(t1);
            int engaged = 0;
            for (int b = 0; b < kBins; ++b) {
                const double th = (-89.5 + b) * kPi / 180;
                const double ux = dir.x * std::cos(th) - dir.y * std::sin(th);
                const double uy = dir.x * std::sin(th) + dir.y * std::cos(th);
                const double x = pos.x + rCut_ * ux, y = pos.y + rCut_ * uy;
                const int i = static_cast<int>(std::lround((x - stock_.x(0)) / hs));
                const int j = static_cast<int>(std::lround((y - stock_.y(0)) / hs));
                if (i < 0 || j < 0 || i >= stock_.nx() || j >= stock_.ny()) {
                    continue;
                }
                for (const StockGrid::SIv& iv : stock_.at(stock_.index(i, j))) {
                    if (iv.hi > pos.z + kStockEps && iv.lo < pos.z + lf_) {
                        ++engaged;
                        break;
                    }
                }
            }
            const double deg = engaged * 180.0 / kBins;
            if (deg > worst) {
                worst = deg;
                at = pos;
            }
            stockPass(whole.part(t0, t1));
        }
        if (worst > *c_.maxEngagementDeg) {
            add(whole, "engagement", worst, *c_.maxEngagementDeg, at,
                "the tool engages " + num(worst) + " degrees of stock");
        }
    }

    // Climb or conventional along walls: which side of the cut the part's wall is on.
    void direction(const Piece& p)
    {
        const double rho = rCut_ + c_.stockToLeaveWall + c_.directionBand;
        const double h = pm_.cell();
        const Vec3 pos = p.at(0.5);
        const Vec2 dir = p.direction(0.5);
        const int i0 = std::max(0, static_cast<int>(std::floor((pos.x - rho - pm_.x0()) / h)));
        const int i1 = std::min(pm_.nx() - 1, static_cast<int>(std::floor((pos.x + rho - pm_.x0()) / h)));
        const int j0 = std::max(0, static_cast<int>(std::floor((pos.y - rho - pm_.y0()) / h)));
        const int j1 = std::min(pm_.ny() - 1, static_cast<int>(std::floor((pos.y + rho - pm_.y0()) / h)));
        double left = kInf, right = kInf;
        std::vector<Iv> zs;
        const std::array<AllowedCut, 0> none{};
        for (int j = j0; j <= j1; ++j) {
            for (int i = i0; i <= i1; ++i) {
                const double x = pm_.centreX(i), y = pm_.centreY(j);
                const double dx = x - pos.x, dy = y - pos.y;
                const double d = std::hypot(dx, dy);
                if (d > rho || d == 0) {
                    continue;
                }
                const double cross = (dir.x * dy - dir.y * dx) / d; // sine of the angle: > 0 left
                if (std::abs(cross) < 0.5) {
                    continue; // ahead or behind, not beside
                }
                pm_.zset(i, j, x, y, h / 2, none, zs);
                bool wall = false;
                for (const Iv& iv : zs) {
                    wall = wall || (iv.hi > pos.z + c_.tolerance && iv.lo < pos.z + lf_);
                }
                if (!wall) {
                    continue;
                }
                (cross > 0 ? left : right) = std::min(cross > 0 ? left : right, d);
            }
        }
        if (std::isinf(left) == std::isinf(right)) {
            return; // no wall, or walls on both sides (a slot)
        }
        const bool wallRight = std::isinf(left);
        const bool climb = wallRight == c_.clockwise; // clockwise spindle: climb keeps the wall on the right
        const bool wantClimb = c_.direction == Direction::Climb;
        if (climb != wantClimb) {
            add(p, "direction", 0, 1, pos,
                std::string("the cut along this wall is ") + (climb ? "climb" : "conventional") + "; " +
                    (wantClimb ? "climb" : "conventional") + " was asked for");
        }
    }

    // ----- Region claims, on the stock samples once every move is done -----

    struct Reach {
        std::vector<double> obstacleDist; // mm, to the nearest sample with part material in the claim's range
        std::vector<std::uint8_t> reachable;
    };

    // Samples a disc of the cutting radius can sweep without the part (material whose Z range meets (zLo, zHi])
    // coming nearer than the stock to leave: the disc's centre at least rCut + stl + hs from every such sample, the
    // sample within rCut - hs of such a centre.
    Reach reach(double zLo, double zHi)
    {
        const int nx = stock_.nx(), ny = stock_.ny();
        const double hs = stock_.cell();
        std::vector<std::uint8_t> obstacle(static_cast<std::size_t>(nx) * ny, 0);
        std::vector<Iv> ray;
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                pm_.ray(stock_.x(i), stock_.y(j), ray);
                for (const Iv& iv : ray) {
                    if (iv.hi > zLo + 1e-6 && iv.lo < zHi) {
                        obstacle[static_cast<std::size_t>(stock_.index(i, j))] = 1;
                    }
                }
            }
        }
        Reach r;
        r.obstacleDist = distanceTo(obstacle, nx, ny);
        std::vector<std::uint8_t> free(obstacle.size(), 0);
        const double need = (rCut_ + c_.stockToLeaveWall + hs) / hs;
        for (std::size_t k = 0; k < free.size(); ++k) {
            free[k] = r.obstacleDist[k] >= need ? 1 : 0;
            r.obstacleDist[k] *= hs;
        }
        const std::vector<double> toFree = distanceTo(free, nx, ny);
        r.reachable.assign(free.size(), 0);
        for (std::size_t k = 0; k < free.size(); ++k) {
            r.reachable[k] = toFree[k] * hs <= rCut_ - hs ? 1 : 0;
        }
        return r;
    }

    void clear(const ClearClaim& cl)
    {
        const double target = cl.floorZ + c_.stockToLeaveFloor + cl.allowedAbove;
        const Reach r = reach(cl.floorZ, kInf);
        const double hs = stock_.cell();
        std::size_t count = 0;
        double worst = 0;
        Vec3 at{};
        for (int j = 0; j < stock_.ny(); ++j) {
            for (int i = 0; i < stock_.nx(); ++i) {
                const int idx = stock_.index(i, j);
                if (!r.reachable[static_cast<std::size_t>(idx)] ||
                    !cl.region.contains(stock_.x(i), stock_.y(j), in_.geom.stockBounds)) {
                    continue;
                }
                const double top = stock_.top(idx);
                if (top > target + 1e-6) {
                    ++count;
                    if (top - target > worst) {
                        worst = top - target;
                        at = Vec3{stock_.x(i), stock_.y(j), top};
                    }
                }
            }
        }
        if (count > 0) {
            sink_.add(Finding{cl.id, in_.path.moves.size(), at, worst + cl.allowedAbove, cl.allowedAbove,
                              std::to_string(count) + " reachable sample(s) (" + num(count * hs * hs) +
                                  " mm2) keep stock above Z " + num(target) + " (stock to leave " +
                                  num(c_.stockToLeaveFloor) + " included)"});
        }
    }

    void walls(const WallClaim& wc)
    {
        const double hs = stock_.cell();
        const double bound = std::max(wc.bound, 2 * hs);
        const Reach r = reach(wc.zLo, wc.zHi);
        const double near = c_.stockToLeaveWall + bound, far = near + rCut_;
        std::size_t count = 0;
        double worst = 0;
        Vec3 at{};
        for (int j = 0; j < stock_.ny(); ++j) {
            for (int i = 0; i < stock_.nx(); ++i) {
                const int idx = stock_.index(i, j);
                const double d = r.obstacleDist[static_cast<std::size_t>(idx)];
                if (!r.reachable[static_cast<std::size_t>(idx)] || d < near || d > far ||
                    !wc.region.contains(stock_.x(i), stock_.y(j), in_.geom.stockBounds)) {
                    continue;
                }
                for (const StockGrid::SIv& iv : stock_.at(idx)) {
                    const double lo = std::max<double>(iv.lo, wc.zLo + c_.tolerance),
                                 hi = std::min<double>(iv.hi, wc.zHi);
                    if (hi > lo + 1e-6) {
                        ++count;
                        const double left = d - c_.stockToLeaveWall; // how far from the wall the stock still is
                        if (left > worst) {
                            worst = left;
                            at = Vec3{stock_.x(i), stock_.y(j), hi};
                        }
                        break;
                    }
                }
            }
        }
        if (count > 0) {
            sink_.add(Finding{"stockLeft", in_.path.moves.size(), at, worst, bound,
                              std::to_string(count) + " reachable sample(s) more than " + num(bound) +
                                  " mm beyond the stock to leave from the walls keep stock between Z " + num(wc.zLo) +
                                  " and " + num(wc.zHi)});
        }
    }

    const CheckInput& in_;
    const Claims& c_;
    const CheckOptions& o_;
    const PartModel& pm_;
    Sink& sink_;
    ToolProfile prof_;
    PartCheck partCheck_;
    StockGrid stock_;
    double rCut_ = 0, lf_ = 0;
    bool centreCutting_ = false;
    std::vector<std::pair<double, double>> runs_;
};

} // namespace

bool Region::contains(double x, double y, const Box3& stock) const
{
    if (stockBox) {
        return x >= stock.min.x && x <= stock.max.x && y >= stock.min.y && y <= stock.max.y;
    }
    bool inside = false;
    for (const auto& loop : loops) {
        if (loop.size() < 3) {
            continue;
        }
        for (std::size_t k = 0, l = loop.size() - 1; k < loop.size(); l = k++) {
            const Vec2 a = loop[k], b = loop[l];
            if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) {
                inside = !inside;
            }
        }
    }
    return inside;
}

double Region::boundaryDistance(double x, double y, const Box3& stock) const
{
    const auto seg = [&](Vec2 a, Vec2 b) {
        const double vx = b.x - a.x, vy = b.y - a.y;
        const double len2 = vx * vx + vy * vy;
        const double t = len2 > 0 ? std::clamp(((x - a.x) * vx + (y - a.y) * vy) / len2, 0.0, 1.0) : 0.0;
        return std::hypot(x - (a.x + t * vx), y - (a.y + t * vy));
    };
    double d = kInf;
    if (stockBox) {
        const Vec2 c[4] = {{stock.min.x, stock.min.y},
                           {stock.max.x, stock.min.y},
                           {stock.max.x, stock.max.y},
                           {stock.min.x, stock.max.y}};
        for (int k = 0; k < 4; ++k) {
            d = std::min(d, seg(c[k], c[(k + 1) % 4]));
        }
        return d;
    }
    for (const auto& loop : loops) {
        for (std::size_t k = 0; k < loop.size(); ++k) {
            d = std::min(d, seg(loop[k], loop[(k + 1) % loop.size()]));
        }
    }
    return d;
}

bool centreCuttingByDefault(lib::ToolType t)
{
    using T = lib::ToolType;
    return t == T::Drill || t == T::SpotDrill || t == T::BallEndMill || t == T::ChamferMill || t == T::EngraveCutter;
}

Claims claimsFor(const Operation& op, const ResolvedGeometry& geom)
{
    Claims c;
    c.spindleRpm = op.cutting.spindleRpm;
    c.clockwise = op.cutting.clockwise;
    c.stickOut = op.tool.stickOut;
    c.rapidFloorZ = geom.stockBounds.max.z;
    return c;
}

std::string describe(const Finding& f)
{
    std::ostringstream s;
    s << "move " << f.move << " [" << f.id << "] " << f.message << " (measured " << num(f.measured) << ", allowed "
      << num(f.allowed) << ") at " << pt(f.at);
    return s.str();
}

std::string describe(const std::vector<Finding>& v, std::size_t max)
{
    std::ostringstream s;
    for (std::size_t k = 0; k < v.size() && k < max; ++k) {
        s << "  " << describe(v[k]) << "\n";
    }
    if (v.size() > max) {
        s << "  ... and " << v.size() - max << " more\n";
    }
    return s.str();
}

ErrorBounds errorBounds(const CheckOptions& o, const Box3& stock)
{
    ErrorBounds e;
    const double longer = std::max(stock.max.x - stock.min.x, stock.max.y - stock.min.y);
    e.stockCell = o.stockCell > 0 ? o.stockCell : std::max(0.1, longer / 1500);
    e.partBelow = o.minCell * std::sqrt(2.0) + o.chord + kChordSagittaMm;
    e.partAbove = o.chord + kChordSagittaMm;
    e.stockMiss = e.stockCell * std::sqrt(2.0);
    return e;
}

std::vector<Finding> check(const CheckInput& in, const CheckOptions& options, std::shared_ptr<const PartModel> part)
{
    // RAY_INVARIANTS_TIMING in the environment prints where the time goes (for tuning the grids).
    const bool timing = std::getenv("RAY_INVARIANTS_TIMING") != nullptr;
    auto t = std::chrono::steady_clock::now();
    const auto lap = [&](const char* what) {
        if (timing) {
            const auto now = std::chrono::steady_clock::now();
            std::fprintf(stderr, "invariants timing: %s %.3f s\n", what,
                         std::chrono::duration<double>(now - t).count());
            t = now;
        }
    };
    Sink sink;
    const Motion mo = motionOf(in.path);
    m2Rules(in, mo, sink);
    moveRules(in, mo, sink);
    lap("move rules");
    if (!part) {
        auto built = PartModel::build(in.geom, options.partCell, options.chord);
        if (!built) {
            sink.add(Finding{"mesh", in.path.moves.size(), Vec3{}, 0, 0,
                             "the part does not mesh: " + built.error().message});
            return sink.take();
        }
        part = *built;
        lap("part mesh and grid");
    }
    Geometry g(in, options, *part, sink);
    lap("stock grid");
    g.run(mo, lap);
    return sink.take();
}

} // namespace ray::cam::invariants
