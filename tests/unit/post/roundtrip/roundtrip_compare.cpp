// The T-043 comparison: the toolpath IR against the interpreted program. See roundtrip_compare.h.
#include "roundtrip_compare.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace rtgc {

namespace {

constexpr double kPi = 3.14159265358979323846;
// The largest chord error accepted where an arc is written as lines (the host's linearisation): 0.01 mm plus the
// least count. [ASSUMPTION: the host's own chord tolerance is not in the post API the comparison may read]
constexpr double kChordLimitMm = 0.01;

double mmOf(ray::Nm v)
{
    return static_cast<double>(v) / 1e6;
}

bool isHomeZ(double z)
{
    return z >= kHomeZ * 0.5;
}

std::string num(double v)
{
    if (std::isnan(v)) {
        return "unknown";
    }
    if (isHomeZ(v)) {
        return "machine zero";
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') {
        s.pop_back();
    }
    if (!s.empty() && s.back() == '.') {
        s.pop_back();
    }
    return s == "-0" ? "0" : s;
}
std::string pt(const P3& p)
{
    return "(" + num(p.x) + ", " + num(p.y) + ", " + num(p.z) + ")";
}

bool near1(double a, double b, double tol)
{
    if (std::isnan(a) || std::isnan(b)) {
        return std::isnan(a) && std::isnan(b);
    }
    if (isHomeZ(a) || isHomeZ(b)) {
        return isHomeZ(a) && isHomeZ(b);
    }
    return std::fabs(a - b) <= tol + 1e-9;
}
bool near3(const P3& a, const P3& b, double tol)
{
    return near1(a.x, b.x, tol) && near1(a.y, b.y, tol) && near1(a.z, b.z, tol);
}
bool sameXY(const P3& a, const P3& b, double tol)
{
    return near1(a.x, b.x, tol) && near1(a.y, b.y, tol);
}
// A Z for "how high": machine zero is above everything, unknown below everything.
double height(double z)
{
    if (std::isnan(z)) {
        return -1e18;
    }
    return z;
}

// In-plane coordinates as in gcode_interp.cpp's convention (counter-clockwise = positive rotation about the plane's
// normal), written again here so the comparison does not lean on the interpreter's code.
void planeCoords(int plane, const P3& p, double& u, double& v, double& a)
{
    if (plane == 17) {
        u = p.x, v = p.y, a = p.z;
    } else if (plane == 18) {
        u = p.z, v = p.x, a = p.y;
    } else {
        u = p.y, v = p.z, a = p.x;
    }
}

double ccwAngle(double from, double to)
{
    double d = to - from;
    while (d < 0) {
        d += 2 * kPi;
    }
    while (d >= 2 * kPi) {
        d -= 2 * kPi;
    }
    return d;
}

enum class PK { Rapid, Feed, Arc, Dwell };

struct Prim {
    PK kind = PK::Rapid;
    P3 from, to;
    P3 centre;
    int plane = 17;
    bool cw = false;
    double sweep = 0;
    double feed = 0;
    double dwell = 0;
    bool mustMatch = false; // a canned cycle's return to its R plane / initial level
    int section = 0;
    int move = -1; // the IR move index; -1 for the move to the toolpath start
    int comp = 0;
    int dir = 1; // expected spindle direction
    bool cycle = false;
    // Peck depths are sums of rounded Q (and J) words: the k-th peck's end point may be off by k + 1 least counts.
    double tolScale = 1;     // for the end point
    double fromTolScale = 1; // for the start point
    std::string what;
};

std::string kindName(ray::cam::MoveKind k)
{
    using K = ray::cam::MoveKind;
    switch (k) {
    case K::Rapid:
        return "rapid";
    case K::Linear:
        return "line";
    case K::ArcCW:
        return "clockwise arc";
    case K::ArcCCW:
        return "counter-clockwise arc";
    case K::Dwell:
        return "dwell";
    case K::CycleDrill:
        return "drill hole";
    case K::CyclePeck:
        return "peck hole";
    case K::CycleChipBreak:
        return "chip-break hole";
    case K::CycleTap:
        return "tapped hole";
    case K::CycleBore:
        return "bored hole";
    case K::CycleReam:
        return "reamed hole";
    case K::CycleCancel:
        return "cycle cancel";
    }
    return "move";
}

// The IR as motion. Cycles follow cam.h "Canned cycles", steps 1 to 4.
std::vector<Prim> expand(const Expectation& e)
{
    using K = ray::cam::MoveKind;
    std::vector<Prim> out;
    P3 cur; // unknown before the first section
    for (std::size_t si = 0; si < e.sections.size(); ++si) {
        const ExpectedSection& sec = e.sections[si];
        const ray::cam::Toolpath& path = *sec.path;
        const int secDir = sec.clockwise ? 1 : -1;
        auto add = [&](Prim p) {
            p.section = static_cast<int>(si);
            if (p.dir == 0) {
                p.dir = secDir;
            }
            out.push_back(p);
            cur = p.to;
        };
        {
            Prim p;
            p.kind = PK::Rapid;
            p.from = cur;
            p.to = P3{mmOf(path.startX), mmOf(path.startY), mmOf(path.startZ)};
            p.what = "section " + std::to_string(si + 1) + " '" + sec.name + "': the toolpath start";
            p.dir = secDir;
            add(p);
        }
        for (std::size_t k = 0; k < path.moves.size(); ++k) {
            const ray::cam::Move& m = path.moves[k];
            const std::string what = "section " + std::to_string(si + 1) + " '" + sec.name + "' move " +
                                     std::to_string(k) + " (" + kindName(m.kind) + ")";
            const P3 end{mmOf(m.x), mmOf(m.y), mmOf(m.z)};
            Prim base;
            base.move = static_cast<int>(k);
            base.what = what;
            base.dir = secDir;
            switch (m.kind) {
            case K::Rapid:
            case K::Linear: {
                Prim p = base;
                p.kind = m.kind == K::Rapid ? PK::Rapid : PK::Feed;
                p.from = cur;
                p.to = end;
                p.feed = m.feedMicronPerMin / 1000.0;
                p.comp = m.compensation;
                add(p);
                break;
            }
            case K::ArcCW:
            case K::ArcCCW: {
                Prim p = base;
                p.kind = PK::Arc;
                p.from = cur;
                p.to = end;
                p.cw = m.kind == K::ArcCW;
                p.plane = m.plane == ray::cam::ArcPlane::XY ? 17 : (m.plane == ray::cam::ArcPlane::XZ ? 18 : 19);
                p.centre = P3{mmOf(m.i), mmOf(m.j), mmOf(m.k)};
                p.feed = m.feedMicronPerMin / 1000.0;
                p.comp = m.compensation;
                double u0, v0, a0, u1, v1, a1, cu, cv, ca;
                planeCoords(p.plane, p.from, u0, v0, a0);
                planeCoords(p.plane, p.to, u1, v1, a1);
                planeCoords(p.plane, p.centre, cu, cv, ca);
                if (m.kind == K::ArcCW || m.kind == K::ArcCCW) {
                    const double s = std::atan2(v0 - cv, u0 - cu), t = std::atan2(v1 - cv, u1 - cu);
                    double sw = p.cw ? ccwAngle(t, s) : ccwAngle(s, t);
                    if (u0 == u1 && v0 == v1) {
                        sw = 2 * kPi; // start = end: a full circle (cam.h)
                    }
                    p.sweep = sw;
                }
                add(p);
                break;
            }
            case K::Dwell: {
                Prim p = base;
                p.kind = PK::Dwell;
                p.from = p.to = cur;
                p.dwell = m.cycleDwellMs / 1000.0;
                add(p);
                break;
            }
            case K::CycleCancel:
                break;
            case K::CycleDrill:
            case K::CyclePeck:
            case K::CycleChipBreak:
            case K::CycleTap:
            case K::CycleBore:
            case K::CycleReam: {
                const double r = mmOf(m.cycleRetract), clear = mmOf(m.cycleClearance), bottom = end.z;
                const double ret = m.cycleReturn == ray::cam::CycleReturn::Initial ? clear : r;
                const double feed = m.feedMicronPerMin / 1000.0;
                const double c = mmOf(ray::cam::peckClearanceNm(m, e.machine));
                Prim b = base;
                b.cycle = true;
                auto rapidTo = [&](P3 to, bool must = false) {
                    if (to.x == cur.x && to.y == cur.y && to.z == cur.z) {
                        return;
                    }
                    Prim p = b;
                    p.kind = PK::Rapid;
                    p.from = cur;
                    p.to = to;
                    p.mustMatch = must;
                    add(p);
                };
                auto feedTo = [&](double z, int dir = 0) {
                    Prim p = b;
                    p.kind = PK::Feed;
                    p.from = cur;
                    p.to = P3{cur.x, cur.y, z};
                    p.feed = feed;
                    if (dir != 0) {
                        p.dir = dir;
                    }
                    add(p);
                };
                auto dwell = [&]() {
                    if (m.cycleDwellMs > 0) {
                        Prim p = b;
                        p.kind = PK::Dwell;
                        p.from = p.to = cur;
                        p.dwell = m.cycleDwellMs / 1000.0;
                        add(p);
                    }
                };
                // 1. X/Y at the current Z; 2. down to the clearance if above it; 3. to R.
                rapidTo(P3{end.x, end.y, cur.z});
                if (cur.z > clear) {
                    rapidTo(P3{end.x, end.y, clear});
                }
                rapidTo(P3{end.x, end.y, r});
                switch (m.kind) {
                case K::CycleDrill:
                    feedTo(bottom);
                    dwell();
                    rapidTo(P3{end.x, end.y, ret}, true);
                    break;
                case K::CyclePeck:
                case K::CycleChipBreak: {
                    const double q = mmOf(m.cyclePeck);
                    double depth = r;
                    double peck = q;
                    const bool reduce = m.kind == K::CyclePeck && e.peckReductionJ >= 0 && q > 0;
                    const double jj = reduce ? e.peckReductionJ * q : 0, kk = reduce ? e.peckMinimumK * q : 0;
                    if (q <= 0) {
                        feedTo(bottom); // cam.h: Q = 0 drills in one feed
                        depth = bottom;
                    }
                    // The rounding of Q (and of J) adds up over the pecks: after n pecks the depth is a sum of n
                    // rounded Q words (n (n + 1) / 2 with the J reduction), each off by up to a least count.
                    auto scale = [&](int n) {
                        return 1.0 + (reduce ? n * (n + 1.0) / 2.0 : n);
                    };
                    int n = 0;
                    while (depth > bottom + 1e-9) {
                        b.tolScale = b.fromTolScale = scale(n);
                        if (depth < r) {
                            if (m.kind == K::CyclePeck) {
                                rapidTo(P3{end.x, end.y, r});
                            }
                            rapidTo(P3{end.x, end.y, std::min(depth + c, r)});
                        }
                        const double next = std::max(depth - peck, bottom);
                        b.tolScale = next <= bottom ? 1.0 : scale(n + 1); // the bottom is a Z word of its own
                        feedTo(next);
                        depth = next;
                        ++n;
                        if (reduce) {
                            peck = std::max(peck - jj, kk);
                        }
                    }
                    b.tolScale = b.fromTolScale = 1;
                    dwell();
                    rapidTo(P3{end.x, end.y, ret}, true);
                    break;
                }
                case K::CycleTap: {
                    const int in = m.cycleLeftHand != 0 ? -1 : 1;
                    feedTo(bottom, in);
                    dwell();
                    feedTo(r, -in);
                    rapidTo(P3{end.x, end.y, ret}, true);
                    break;
                }
                default: // bore, ream
                    feedTo(bottom);
                    dwell();
                    feedTo(r);
                    rapidTo(P3{end.x, end.y, ret}, true);
                    break;
                }
                break;
            }
            }
        }
    }
    return out;
}

class Matcher {
public:
    Matcher(const Trace& t, const Expectation& e, const std::vector<Prim>& ir) : t_(t), e_(e), ir_(ir) {}

    std::vector<Issue> run()
    {
        while (true) {
            const std::size_t i0 = i_, j0 = j_;
            while (i_ < ir_.size() && ir_[i_].kind == PK::Rapid) {
                ++i_;
            }
            while (j_ < segs().size() && segs()[j_].kind == SegKind::Rapid) {
                ++j_;
            }
            const Prim* anchor = i_ < ir_.size() ? &ir_[i_] : nullptr;
            if (!gap(i0, i_, j0, j_, anchor)) {
                break;
            }
            if (anchor == nullptr) {
                if (j_ < segs().size()) {
                    issue(segs()[j_].line, "rt-extra-motion", "cutting motion after the end of the toolpath");
                }
                break;
            }
            if (j_ >= segs().size()) {
                issue(0, "rt-missing-motion", "the program ends before " + anchor->what);
                break;
            }
            if (!matchAnchor(*anchor)) {
                break;
            }
            ++i_;
        }
        checkWarnings();
        return out_;
    }

private:
    const Trace& t_;
    const Expectation& e_;
    const std::vector<Prim>& ir_;
    std::size_t i_ = 0, j_ = 0;
    std::vector<Issue> out_;
    std::set<std::string> reported_;
    std::set<std::pair<int, int>> native_; // (section, move) of IR cycle moves the program writes as native cycles

    const std::vector<Segment>& segs() const { return t_.segs; }
    void issue(int line, const std::string& code, const std::string& text) { out_.push_back(Issue{line, code, text}); }

    // Rapids between two cuts (see roundtrip_compare.h).
    bool gap(std::size_t i0, std::size_t i1, std::size_t j0, std::size_t j1, const Prim* anchor)
    {
        double top = anchor != nullptr ? height(anchor->from.z) : -1e18;
        std::vector<P3> waypoints;
        std::vector<double> waypointScale;
        for (std::size_t i = i0; i < i1; ++i) {
            top = std::max(top, height(ir_[i].to.z));
            if (!std::isnan(ir_[i].from.z)) {
                top = std::max(top, height(ir_[i].from.z));
            }
            waypoints.push_back(ir_[i].to);
            waypointScale.push_back(ir_[i].tolScale);
        }
        if (anchor != nullptr) {
            waypoints.push_back(anchor->from);
            waypointScale.push_back(anchor->fromTolScale);
        }
        std::vector<bool> used(i1 - i0, false);
        std::size_t nextMust = i0;
        auto advanceMust = [&]() {
            while (nextMust < i1 && !ir_[nextMust].mustMatch) {
                ++nextMust;
            }
        };
        advanceMust();
        for (std::size_t j = j0; j < j1; ++j) {
            const Segment& s = segs()[j];
            const double tol = s.tol;
            bool exact = false;
            for (std::size_t i = i0; i < i1; ++i) {
                if (!used[i - i0] && near3(ir_[i].from, s.from, tol * ir_[i].fromTolScale) &&
                    near3(ir_[i].to, s.to, tol * ir_[i].tolScale)) {
                    used[i - i0] = true;
                    exact = true;
                    break;
                }
            }
            if (nextMust < i1 && near3(ir_[nextMust].to, s.to, tol)) {
                ++nextMust;
                advanceMust();
            }
            if (exact) {
                continue;
            }
            const bool xy = !sameXY(s.from, s.to, tol);
            const double hFrom = height(s.from.z), hTo = height(s.to.z);
            if (xy) {
                const double low = std::min(hFrom, hTo);
                if (low < top - tol - 1e-9) {
                    issue(s.line, "rt-unsafe-rapid",
                          "a rapid moves X/Y at Z " + num(std::min(s.from.z, s.to.z)) +
                              ", below the toolpath's travel height Z " + num(top) + " here (from " + pt(s.from) +
                              " to " + pt(s.to) + ")");
                } else if (!std::isnan(s.to.x) && !std::isnan(s.to.y)) {
                    // Travel the post adds goes to where the toolpath goes next (or to machine home, unknown here).
                    bool toPoint = false;
                    for (std::size_t k = 0; k < waypoints.size(); ++k) {
                        toPoint = toPoint || sameXY(waypoints[k], s.to, tol * waypointScale[k]);
                    }
                    if (!toPoint) {
                        issue(s.line, "rt-travel",
                              "a rapid travels to X " + num(s.to.x) + " Y " + num(s.to.y) +
                                  ", which is not a point of the toolpath here");
                    }
                }
            } else if (hTo < hFrom || std::isnan(s.from.z)) {
                bool onPoint = false;
                for (std::size_t k = 0; k < waypoints.size(); ++k) {
                    onPoint = onPoint || near3(waypoints[k], s.to, tol * waypointScale[k]);
                }
                if (!onPoint && hTo < top - tol - 1e-9) {
                    issue(s.line, "rt-unsafe-rapid",
                          "a rapid descends to Z " + num(s.to.z) +
                              ", which is not a toolpath point and is below the "
                              "toolpath's travel height Z " +
                              num(top) + " here");
                }
            }
        }
        if (nextMust < i1) {
            const Prim& p = ir_[nextMust];
            // Name the hole's block: the first cycle segment of the gap, else the cycle feed just before it.
            int line = j1 > j0 ? segs()[j0].line : (j1 < segs().size() ? segs()[j1].line : 0);
            if (j0 > 0 && segs()[j0 - 1].origin == SegOrigin::Cycle) {
                line = segs()[j0 - 1].line;
            }
            for (std::size_t j = j0; j < j1; ++j) {
                if (segs()[j].origin == SegOrigin::Cycle) {
                    line = segs()[j].line;
                    break;
                }
            }
            issue(line, "rt-cycle-return",
                  p.what + " returns to Z " + num(p.to.z) + " (cam.h cycleReturn); the program never goes there");
        }
        if (anchor != nullptr && j1 < segs().size()) {
            const Segment& s = segs()[j1];
            if (!near3(s.from, anchor->from, s.tol * anchor->fromTolScale)) {
                issue(s.line, "rt-position",
                      "the program is at " + pt(s.from) + " where " + anchor->what + " starts at " + pt(anchor->from));
                return false;
            }
        }
        return true; // unsafe rapids are reported and the matching goes on; a wrong position stops it
    }

    void checkState(const Segment& s, const Prim& p)
    {
        const ExpectedSection& sec = e_.sections[static_cast<std::size_t>(p.section)];
        auto once = [&](const std::string& code, const std::string& text) {
            if (reported_.insert(code + "/" + std::to_string(p.section)).second) {
                issue(s.line, code, text + " (" + p.what + ")");
            }
        };
        const Modal& m = s.state;
        if (m.tool != sec.tool) {
            once("rt-tool", "tool T" + std::to_string(m.tool) + " is in the spindle; the operation uses T" +
                                std::to_string(sec.tool));
        }
        if (m.lengthH != sec.lengthOffset) {
            once("rt-length-offset",
                 (m.lengthH < 0 ? std::string("no length offset") : "length offset H" + std::to_string(m.lengthH)) +
                     " is active; the operation uses H" + std::to_string(sec.lengthOffset));
        }
        if (m.comp != p.comp) {
            once("rt-comp", std::string("cutter compensation is ") +
                                (m.comp == 0 ? "off" : (m.comp == 1 ? "left (G41)" : "right (G42)")) +
                                "; the toolpath has " +
                                (p.comp == 0 ? "off" : (p.comp == 1 ? "left (G41)" : "right (G42)")));
        } else if (m.comp != 0 && m.dNumber != sec.diameterOffset) {
            once("rt-d", "diameter offset D" + std::to_string(m.dNumber) + "; the operation uses D" +
                             std::to_string(sec.diameterOffset));
        }
        if (std::fabs(m.rpm - sec.rpm) > 0.5) {
            once("rt-speed", "spindle speed S" + num(m.rpm) + "; the operation uses " + num(sec.rpm) + " rpm");
        }
        if (m.spindle != p.dir) {
            once("rt-direction",
                 std::string("the spindle turns ") +
                     (m.spindle == 0 ? "not at all" : (m.spindle > 0 ? "clockwise" : "counter-clockwise")) +
                     "; the toolpath needs it " + (p.dir > 0 ? "clockwise" : "counter-clockwise"));
        }
        if (m.coolant != sec.coolant) {
            once("rt-coolant", "coolant state " + std::to_string(m.coolant) + "; the operation uses " +
                                   std::to_string(sec.coolant) + " (bits: 1 flood, 2 mist, 4 through, 8 air)");
        }
        if (!sec.wcs.empty() && m.wcs != sec.wcs) {
            once("rt-wcs", "work offset " + m.wcs + "; the setup uses " + sec.wcs);
        }
    }

    bool checkFeed(const Segment& s, const Prim& p)
    {
        if (std::fabs(s.feed - p.feed) > s.feedTol + 1e-9) {
            issue(s.line, "rt-feed", "feed " + num(s.feed) + " mm/min; " + p.what + " has " + num(p.feed) + " mm/min");
            return false;
        }
        return true;
    }

    bool matchAnchor(const Prim& p)
    {
        const Segment& first = segs()[j_];
        if (p.cycle && first.origin == SegOrigin::Cycle) {
            native_.insert({p.section, p.move});
        }
        if (p.kind == PK::Dwell) {
            if (first.kind != SegKind::Dwell) {
                issue(first.line, "rt-dwell", "the program moves where " + p.what + " dwells " + num(p.dwell) + " s");
                return false;
            }
            if (std::fabs(first.dwell - p.dwell) > first.dwellTol + 1e-9) {
                issue(first.line, "rt-dwell",
                      "a dwell of " + num(first.dwell) + " s where " + p.what + " dwells " + num(p.dwell) + " s");
                ++j_;
                return true;
            }
            ++j_;
            return true;
        }
        if (first.kind == SegKind::Dwell) {
            issue(first.line, "rt-dwell", "a dwell the toolpath does not have (expected " + p.what + ")");
            return false;
        }
        if (p.kind == PK::Feed) {
            return matchLine(p);
        }
        return matchArc(p);
    }

    bool matchLine(const Prim& p)
    {
        const double dx = p.to.x - p.from.x, dy = p.to.y - p.from.y, dz = p.to.z - p.from.z;
        const double len2 = dx * dx + dy * dy + dz * dz;
        double lastT = 0;
        while (j_ < segs().size()) {
            const Segment& s = segs()[j_];
            if (s.kind != SegKind::Feed) {
                issue(s.line, "rt-shape",
                      std::string(s.kind == SegKind::Arc ? "an arc" : "a rapid") + " where " + p.what +
                          " is a straight feed to " + pt(p.to));
                return false;
            }
            const double tol = s.tol * p.tolScale;
            double t = 1;
            double dist = 0;
            if (len2 > 0) {
                t = ((s.to.x - p.from.x) * dx + (s.to.y - p.from.y) * dy + (s.to.z - p.from.z) * dz) / len2;
                const double px = p.from.x + t * dx - s.to.x, py = p.from.y + t * dy - s.to.y,
                             pz = p.from.z + t * dz - s.to.z;
                dist = std::sqrt(px * px + py * py + pz * pz);
            }
            if (dist > 2 * tol + 1e-9 || t < lastT - 1e-9 || t > 1 + 1e-9 + tol / std::sqrt(std::max(len2, 1e-18))) {
                issue(s.line, "rt-end-point",
                      "the feed ends at " + pt(s.to) + "; " + p.what + " goes from " + pt(p.from) + " to " + pt(p.to));
                return false;
            }
            checkFeed(s, p);
            checkState(s, p);
            ++j_;
            lastT = t;
            if (near3(s.to, p.to, tol)) {
                return true;
            }
        }
        issue(0, "rt-missing-motion", "the program ends inside " + p.what);
        return false;
    }

    bool matchArc(const Prim& p)
    {
        double u0, v0, a0, u1, v1, a1, cu, cv, ca;
        planeCoords(p.plane, p.from, u0, v0, a0);
        planeCoords(p.plane, p.to, u1, v1, a1);
        planeCoords(p.plane, p.centre, cu, cv, ca);
        const double r = std::hypot(u0 - cu, v0 - cv);
        const double start = std::atan2(v0 - cv, u0 - cu);
        double acc = 0;
        while (j_ < segs().size()) {
            const Segment& s = segs()[j_];
            const double tol = s.tol;
            const double atol = 2 * tol / std::max(r, 1e-6) + 1e-9;
            double su, sv, sa;
            planeCoords(p.plane, s.to, su, sv, sa);
            if (s.kind == SegKind::Arc) {
                if (s.plane != p.plane) {
                    issue(s.line, "rt-arc-plane",
                          "an arc in G" + std::to_string(s.plane) + " where " + p.what + " is in G" +
                              std::to_string(p.plane));
                    return false;
                }
                if (s.cw != p.cw) {
                    issue(s.line, "rt-arc-direction",
                          std::string("a ") + (s.cw ? "clockwise (G2)" : "counter-clockwise (G3)") + " arc where " +
                              p.what + " turns the other way");
                    return false;
                }
                double scu, scv, sca;
                planeCoords(p.plane, s.centre, scu, scv, sca);
                const double ctol =
                    s.rFormat ? 3 * tol / std::max(std::sin(std::min(s.sweep, kPi) / 2), 0.05) : 2 * tol;
                if (std::hypot(scu - cu, scv - cv) > ctol + 1e-9) {
                    issue(s.line, "rt-arc-centre",
                          "the arc's centre is " + pt(s.centre) + "; " + p.what + " turns about " + pt(p.centre));
                    return false;
                }
                acc += s.sweep;
            } else if (s.kind == SegKind::Feed) {
                // A line of a linearised arc: its end on the arc, the angle moving the arc's way.
                const double ang = std::atan2(sv - cv, su - cu);
                double phi = p.cw ? ccwAngle(ang, start) : ccwAngle(start, ang);
                while (phi < acc - atol) {
                    phi += 2 * kPi;
                }
                if (std::fabs(std::hypot(su - cu, sv - cv) - r) > 2 * tol + 1e-9) {
                    issue(s.line, "rt-arc-linear",
                          "a line ends off " + p.what + " (radius " + num(std::hypot(su - cu, sv - cv)) + " against " +
                              num(r) + ")");
                    return false;
                }
                const double sag = r * (1 - std::cos((phi - acc) / 2));
                if (sag > kChordLimitMm + tol) {
                    issue(s.line, "rt-arc-linear",
                          "a line cuts " + num(sag) + " mm inside " + p.what + " (chord limit " + num(kChordLimitMm) +
                              " mm)");
                    return false;
                }
                acc = phi;
            } else {
                issue(s.line, "rt-shape", "a rapid inside " + p.what);
                return false;
            }
            // The end of this piece on the helix at the angle reached.
            const double ang = start + (p.cw ? -acc : acc);
            const double eu = cu + r * std::cos(ang), ev = cv + r * std::sin(ang);
            const double ea = a0 + (a1 - a0) * std::min(acc / std::max(p.sweep, 1e-12), 1.0);
            if (std::hypot(su - eu, sv - ev) > 2 * tol + 1e-9 || std::fabs(sa - ea) > 2 * tol + 1e-9) {
                issue(s.line, "rt-arc-end",
                      "the arc reaches " + pt(s.to) + ", off " + p.what + " (from " + pt(p.from) + " to " + pt(p.to) +
                          " about " + pt(p.centre) + ")");
                return false;
            }
            checkFeed(s, p);
            checkState(s, p);
            ++j_;
            if (acc > p.sweep + atol) {
                issue(s.line, "rt-arc-sweep",
                      "the arcs sweep " + num(acc * 180 / kPi) + " degrees; " + p.what + " sweeps " +
                          num(p.sweep * 180 / kPi));
                return false;
            }
            if (std::fabs(acc - p.sweep) <= atol) {
                if (!near3(s.to, p.to, tol)) {
                    issue(s.line, "rt-end-point",
                          "the arc ends at " + pt(s.to) + "; " + p.what + " ends at " + pt(p.to));
                    return false;
                }
                return true;
            }
        }
        issue(0, "rt-missing-motion", "the program ends inside " + p.what);
        return false;
    }

    // cam.h: a native cycle cannot carry the IR's peck clearance; the post must say so, never drop it silently.
    void checkWarnings()
    {
        using K = ray::cam::MoveKind;
        bool warned = false;
        for (const std::string& w : e_.warnings) {
            warned = warned || w.find("peck clearance") != std::string::npos;
        }
        for (const auto& [section, move] : native_) {
            const ray::cam::Move& m =
                e_.sections[static_cast<std::size_t>(section)].path->moves[static_cast<std::size_t>(move)];
            if ((m.kind == K::CyclePeck || m.kind == K::CycleChipBreak) && m.cyclePeckClearance > 0 && !warned) {
                issue(0, "rt-peck-clearance-silent",
                      "section " + std::to_string(section + 1) + " move " + std::to_string(move) +
                          ": the toolpath's peck clearance is not in the native cycle and no warning says so");
                return;
            }
        }
    }
};

} // namespace

std::string workOffsetName(int workOffset)
{
    if (workOffset >= 1 && workOffset <= 6) {
        return "G" + std::to_string(53 + workOffset);
    }
    return {};
}

std::vector<Issue> compare(const Trace& trace, const Expectation& expected)
{
    const std::vector<Prim> ir = expand(expected);
    Matcher m(trace, expected, ir);
    return m.run();
}

} // namespace rtgc
