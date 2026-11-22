// Offsets and booleans with arcs kept exact (task T-050). geom2d.h "Offsets and booleans" states the contract.
//
// HOW. Each operation first writes down the exact curves its result is made of, as closed "raw" curves of primitives
// (Prim: a line or an arc with an integer centre and radius):
//   - offset: for every span of a boundary its offset (a line moved by delta along its right normal; an arc of radius
//     r + delta or r - delta about the same centre, inverted through the centre when that is negative), and at every
//     vertex either an arc of radius |delta| about the vertex (where the offsets of the two spans open a gap) or two
//     connectors through the vertex (where they overlap), exactly as Clipper2's own offset does with polygons; the
//     region is then the union of the raw curves under the Positive fill rule;
//   - offsetOpen / sweptArea: the path offset to its right, a half circle round its end, the reversed path offset to
//     its right (the original's left) and a half circle round its start;
//   - booleans: the input boundaries themselves.
// The raw curves are flattened with sag `sag` (vertices on the curves, rounded) and every vertex carries the id of
// its primitive in Clipper2's Z (Clipper2Z, built with USINGZ). Clipper2 computes the union or boolean; where it
// creates a vertex at a crossing, the Z callback records the primitives of both crossing edges. Then each output
// polygon is rebuilt:
//   1. each polygon edge is given the candidate primitive (the tags at its two ends, their neighbours on their raw
//      curve, and the primitives recorded at a crossing) whose exact curve its two ends and midpoint are closest to;
//      if none is within 2 sag + 4 nm, the edge is kept literally as a line (it is within sag of the result anyway;
//      rebuildStats counts these, and the tests require none on their cases);
//   2. a single edge given to another primitive between two edges of the same primitive is given back to it when it
//      lies within that tolerance of it (a polygon edge close to a tangent point fits both);
//   3. consecutive edges of one primitive form one span; where two spans meet, the corner is the end of the first
//      primitive when the second follows it on its raw curve (the junction written down when the raw curve was made),
//      otherwise the intersection of the two exact curves nearest Clipper2's vertex (for curves that miss each other
//      by at most the sag near a tangency, their point of closest approach);
//   4. an arc span's direction is the direction the polygon runs round the arc's centre.
// So every line and arc of the result is an exact piece of the right curve, and only which pieces exist and how they
// connect is decided on the flattened polygons. Points are rounded once, at the end (roundNm).
#include "geom2d_internal.h"

#include <clipper2/clipper.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <functional>
#include <limits>
#include <string>
#include <utility>

namespace ray::cam::geom2d {

using namespace detail;

namespace {

constexpr std::string_view kWhere = "cam::geom2d offset/boolean";
constexpr double kTangentSin = 1e-6; // spans meeting at a smaller angle meet tangentially (no join, no connector)

Error invalid(std::string message)
{
    return Error{ErrorCode::InvalidArgument, std::move(message), {std::string(kWhere)}};
}

struct Prim {
    bool arc = false;
    bool whole = false; // a whole circle (a == b)
    D2 a, b;            // start and end
    Pt c{};             // arc centre
    Coord r = 0;        // arc radius > 0
    bool ccw = true;
    int next = -1, prev = -1;
    int group = 0;
    bool degenerate = false; // a line shorter than 1 nm: never given an edge
};

class Raw {
public:
    std::vector<Prim> prims;
    std::vector<std::pair<int, int>> curves; // first and last prim id of each closed raw curve
    int pathOf = 0;

    void begin() { first_ = static_cast<int>(prims.size()); }
    int add(Prim p)
    {
        if (!p.arc && distance(p.a, p.b) < 1.0)
            p.degenerate = true;
        const int id = static_cast<int>(prims.size());
        if (id > first_) {
            prims[static_cast<std::size_t>(id - 1)].next = id;
            p.prev = id - 1;
        }
        prims.push_back(p);
        return id;
    }
    void close()
    {
        const int last = static_cast<int>(prims.size()) - 1;
        if (last < first_)
            return;
        prims[static_cast<std::size_t>(last)].next = first_;
        prims[static_cast<std::size_t>(first_)].prev = last;
        curves.emplace_back(first_, last);
    }

private:
    int first_ = 0;
};

Prim linePrim(D2 a, D2 b, int group)
{
    Prim p;
    p.a = a;
    p.b = b;
    p.group = group;
    return p;
}
Prim arcPrim(Pt c, Coord r, bool ccw, D2 a, D2 b, int group)
{
    Prim p;
    p.arc = true;
    p.c = c;
    p.r = r;
    p.ccw = ccw;
    p.a = a;
    p.b = b;
    p.group = group;
    return p;
}

// ---------- Raw offset curves ----------
// The offset primitives of the spans of a path (s > 0: to the right of travel), with joins between consecutive spans,
// the closing join too when `closed`. Each span's offset is followed by the join at its end.
void offsetChain(Raw& raw, const Path& path, double s, bool closed, int group)
{
    const std::size_t n = path.spans.size();
    std::vector<Pt> starts(n);
    {
        Pt cur = path.start;
        for (std::size_t i = 0; i < n; ++i) {
            starts[i] = cur;
            cur = path.spans[i].end;
        }
    }
    std::vector<Prim> offs(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Span& sp = path.spans[i];
        const D2 a = d2(starts[i]), b = d2(sp.end);
        if (!isArc(sp)) {
            const D2 nrm = rightOf(unit(b - a));
            offs[i] = linePrim(a + nrm * s, b + nrm * s, group);
            continue;
        }
        const D2 c = d2(sp.centre);
        const double r1 = static_cast<double>(sp.radius) + (isCcw(sp) ? s : -s);
        const D2 ua = unit(a - c), ub = unit(b - c);
        if (r1 == 0.0) {
            offs[i] = linePrim(c, c, group);
            continue;
        }
        offs[i] = arcPrim(sp.centre, static_cast<Coord>(std::abs(r1)), isCcw(sp), c + ua * r1, c + ub * r1, group);
    }
    for (std::size_t i = 0; i < n; ++i) {
        raw.add(offs[i]);
        if (i + 1 == n && !closed)
            break;
        const std::size_t j = (i + 1) % n;
        const Span& s1 = path.spans[i];
        const Span& s2 = path.spans[j];
        const Pt v = s1.end;
        const D2 t1 = tangentAtEnd(starts[i], s1);
        const D2 t2 = tangentAtStart(starts[j], s2);
        const double cr = cross(t1, t2), dt = dot(t1, t2);
        const D2 e1 = offs[i].b, s2p = offs[j].a;
        const bool tangent = std::abs(cr) <= kTangentSin;
        if (tangent && dt > 0.0) {
            if (distance(e1, s2p) >= 0.5)
                raw.add(linePrim(e1, s2p, group));
            continue;
        }
        const bool gap = tangent ? true : ((cr > 0.0) == (s > 0.0));
        if (gap) {
            if (distance(e1, s2p) < 0.5)
                continue;
            const bool ccw = tangent ? s > 0.0 : cr > 0.0;
            raw.add(arcPrim(v, static_cast<Coord>(std::abs(s)), ccw, e1, s2p, group));
        } else {
            raw.add(linePrim(e1, d2(v), group));
            raw.add(linePrim(d2(v), s2p, group));
        }
    }
}

// A closed boundary offset by s (s > 0 grows the region on its left).
void addOffsetBoundary(Raw& raw, const Path& path, double s, int group)
{
    if (path.spans.size() == 1 && isArc(path.spans[0])) {
        const Span& sp = path.spans[0];
        const double r1 = static_cast<double>(sp.radius) + (isCcw(sp) ? s : -s);
        if (r1 <= 0.0)
            return;
        raw.begin();
        const D2 a = d2(sp.centre) + D2{r1, 0.0};
        Prim p = arcPrim(sp.centre, static_cast<Coord>(r1), isCcw(sp), a, a, group);
        p.whole = true;
        raw.add(p);
        raw.close();
        return;
    }
    raw.begin();
    offsetChain(raw, path, s, true, group);
    raw.close();
}

// The outline of the area a disc of radius d sweeps along an open path: right side (group `right`), round cap, left
// side (group `left`, running backwards), round cap (group `caps`).
void addSweepOutline(Raw& raw, const Path& path, double d, int right, int left, int caps)
{
    Path open = path;
    open.closed = false;
    raw.begin();
    if (open.spans.empty()) {
        const Pt c = open.start;
        const D2 a = d2(c) + D2{d, 0.0};
        Prim p = arcPrim(c, static_cast<Coord>(d), true, a, a, caps);
        p.whole = true;
        raw.add(p);
        raw.close();
        return;
    }
    const Path back = reverse(open);
    const auto capAt = [&](const Path& forward) {
        const Span& last = forward.spans.back();
        const Pt lastStart = forward.spans.size() >= 2 ? forward.spans[forward.spans.size() - 2].end : forward.start;
        const D2 t = tangentAtEnd(lastStart, last);
        const D2 v = d2(last.end);
        raw.add(arcPrim(last.end, static_cast<Coord>(d), true, v + rightOf(t) * d, v + leftOf(t) * d, caps));
    };
    offsetChain(raw, open, d, false, right);
    capAt(open);
    offsetChain(raw, back, d, false, left);
    capAt(back);
    raw.close();
}

void addBoundary(Raw& raw, const Path& path, int group)
{
    raw.begin();
    if (path.spans.size() == 1 && isArc(path.spans[0])) {
        const Span& sp = path.spans[0];
        Prim p = arcPrim(sp.centre, sp.radius, isCcw(sp), d2(path.start), d2(path.start), group);
        p.whole = true;
        raw.add(p);
        raw.close();
        return;
    }
    Pt cur = path.start;
    for (const Span& sp : path.spans) {
        if (!isArc(sp))
            raw.add(linePrim(d2(cur), d2(sp.end), group));
        else
            raw.add(arcPrim(sp.centre, sp.radius, isCcw(sp), d2(cur), d2(sp.end), group));
        cur = sp.end;
    }
    raw.close();
}

// ---------- Flattening ----------
double primSweep(const Prim& p)
{
    if (p.whole)
        return kTwoPi;
    const D2 c = d2(p.c);
    return sweepBetween(angleOf(p.a, c), angleOf(p.b, c), p.ccw);
}

Clipper2Lib::Path64 flatten(const Raw& raw, std::pair<int, int> curve, Coord sag)
{
    Clipper2Lib::Path64 out;
    for (int id = curve.first; id <= curve.second; ++id) {
        const Prim& p = raw.prims[static_cast<std::size_t>(id)];
        const Pt a = roundPt(p.a);
        out.emplace_back(a.x, a.y, static_cast<std::int64_t>(id));
        if (!p.arc)
            continue;
        const double r = static_cast<double>(p.r);
        const double sweep = primSweep(p);
        const double ratio = std::clamp(1.0 - static_cast<double>(std::max<Coord>(sag, 1)) / r, -1.0, 1.0);
        const double step = std::max(1e-7, std::min(kPi / 4, 2.0 * dmath::acos(ratio)));
        const int count = std::max(p.whole ? 4 : 1, static_cast<int>(std::ceil(sweep / step)));
        const D2 c = d2(p.c);
        const double a0 = angleOf(p.a, c);
        const double dir = p.ccw ? 1.0 : -1.0;
        for (int k = 1; k < count; ++k) {
            const Pt q = roundPt(onCircle(c, r, a0 + dir * sweep * k / count));
            out.emplace_back(q.x, q.y, static_cast<std::int64_t>(id));
        }
    }
    return out;
}

// ---------- Rebuilding ----------
struct Rebuilt {
    Path path;
    std::vector<int> prims; // per span: the primitive, -1 for a literal edge
};

class Engine {
public:
    Engine(const Raw& raw, Coord sag) : raw_(raw), sag_(static_cast<double>(std::max<Coord>(sag, 1))) {}

    // Records a crossing; returns the Z to give it.
    std::int64_t record(const Clipper2Lib::Point64& e1bot, const Clipper2Lib::Point64& e1top,
                        const Clipper2Lib::Point64& e2bot, const Clipper2Lib::Point64& e2top, std::int64_t preset)
    {
        std::array<int, 5> tags{};
        tags[0] = tagOf(e1bot.z);
        tags[1] = tagOf(e1top.z);
        tags[2] = tagOf(e2bot.z);
        tags[3] = tagOf(e2top.z);
        tags[4] = tagOf(preset);
        crossings_.push_back(tags);
        return -static_cast<std::int64_t>(crossings_.size());
    }

    Rebuilt rebuild(const Clipper2Lib::Path64& poly)
    {
        Rebuilt out;
        const std::size_t n = poly.size();
        if (n < 3)
            return out;
        const double limit = 2.0 * sag_ + 4.0;
        // 1. An exact curve for each edge.
        std::vector<int> edge(n, -1);
        std::vector<double> dev(n, 0.0);
        for (std::size_t k = 0; k < n; ++k) {
            const auto& u = poly[k];
            const auto& w = poly[(k + 1) % n];
            candidates_.clear();
            addCandidates(u.z);
            addCandidates(w.z);
            std::sort(candidates_.begin(), candidates_.end());
            candidates_.erase(std::unique(candidates_.begin(), candidates_.end()), candidates_.end());
            int best = -1;
            double bestDev = 0.0;
            for (const int c : candidates_) {
                const double d = deviation(c, u, w);
                if (best < 0 || d < bestDev) {
                    best = c;
                    bestDev = d;
                }
            }
            if (best >= 0 && bestDev <= limit) {
                edge[k] = best;
                dev[k] = bestDev;
            } else {
                ++literalEdges;
            }
        }
        // 2. A lone edge between two edges of one primitive goes back to it when it fits it.
        for (std::size_t k = 0; k < n; ++k) {
            const int before = edge[(k + n - 1) % n], after = edge[(k + 1) % n];
            if (before >= 0 && before == after && edge[k] != before &&
                deviation(before, poly[k], poly[(k + 1) % n]) <= limit)
                edge[k] = before;
        }
        // 3. Spans. Literal edges are spans of their own.
        std::size_t first = n;
        for (std::size_t k = 0; k < n; ++k)
            if (edge[k] != edge[(k + n - 1) % n] || edge[k] < 0) {
                first = k;
                break;
            }
        if (first == n) {
            // One primitive all round: a whole circle (or nothing, for a line).
            const Prim& p = raw_.prims[static_cast<std::size_t>(edge[0])];
            if (p.arc) {
                out.path = Path::circle(p.c, p.r, turnsCcw(p.c, poly, 0, n));
                out.prims.push_back(edge[0]);
            }
            return out;
        }
        struct Group {
            std::size_t from, count;
            int prim;
        };
        std::vector<Group> groups;
        for (std::size_t i = 0; i < n;) {
            const std::size_t k = (first + i) % n;
            std::size_t count = 1;
            if (edge[k] >= 0)
                while (i + count < n && edge[(first + i + count) % n] == edge[k])
                    ++count;
            groups.push_back(Group{k, count, edge[k]});
            i += count;
        }
        // Corners: corner g is where group g starts.
        const std::size_t m = groups.size();
        std::vector<D2> corner(m);
        for (std::size_t g = 0; g < m; ++g) {
            const Group& prev = groups[(g + m - 1) % m];
            const Group& cur = groups[g];
            const auto& x = poly[cur.from];
            corner[g] = junction(prev.prim, cur.prim, D2{static_cast<double>(x.x), static_cast<double>(x.y)});
        }
        out.path.closed = true;
        out.path.start = roundPt(corner[0]);
        Pt at = out.path.start;
        for (std::size_t g = 0; g < m; ++g) {
            const Group& cur = groups[g];
            const Pt end = roundPt(corner[(g + 1) % m]);
            if (end == at)
                continue;
            Span s;
            s.end = end;
            if (cur.prim >= 0 && raw_.prims[static_cast<std::size_t>(cur.prim)].arc) {
                const Prim& p = raw_.prims[static_cast<std::size_t>(cur.prim)];
                s.kind = turnsCcw(p.c, poly, cur.from, cur.count) ? SpanKind::ArcCCW : SpanKind::ArcCW;
                s.centre = p.c;
                s.radius = p.r;
            }
            out.path.spans.push_back(s);
            out.prims.push_back(cur.prim);
            at = end;
        }
        if (!out.path.spans.empty() && out.path.spans.back().end != out.path.start) {
            // The last corner rounded onto the start: close exactly.
            out.path.spans.back().end = out.path.start;
        }
        return out;
    }

    std::size_t literalEdges = 0;

private:
    int tagOf(std::int64_t z) const
    {
        return z >= 0 && z < static_cast<std::int64_t>(raw_.prims.size()) ? static_cast<int>(z) : -1;
    }

    void addPrim(int id)
    {
        if (id < 0 || static_cast<std::size_t>(id) >= raw_.prims.size())
            return;
        const Prim& p = raw_.prims[static_cast<std::size_t>(id)];
        if (!p.degenerate)
            candidates_.push_back(id);
    }
    void addAround(int id)
    {
        if (id < 0 || static_cast<std::size_t>(id) >= raw_.prims.size())
            return;
        const Prim& p = raw_.prims[static_cast<std::size_t>(id)];
        addPrim(id);
        // Neighbours, skipping degenerate ones (a zero-length connector between two real primitives).
        int q = p.prev;
        for (int hop = 0; hop < 3 && q >= 0; ++hop, q = raw_.prims[static_cast<std::size_t>(q)].prev) {
            addPrim(q);
            if (!raw_.prims[static_cast<std::size_t>(q)].degenerate)
                break;
        }
        q = p.next;
        for (int hop = 0; hop < 3 && q >= 0; ++hop, q = raw_.prims[static_cast<std::size_t>(q)].next) {
            addPrim(q);
            if (!raw_.prims[static_cast<std::size_t>(q)].degenerate)
                break;
        }
    }
    void addCandidates(std::int64_t z)
    {
        if (z >= 0) {
            addAround(static_cast<int>(z));
            return;
        }
        const std::size_t index = static_cast<std::size_t>(-z - 1);
        if (index >= crossings_.size())
            return;
        for (const int t : crossings_[index])
            if (t >= 0)
                addAround(t);
    }

    double distanceTo(int id, D2 q) const
    {
        const Prim& p = raw_.prims[static_cast<std::size_t>(id)];
        if (p.arc)
            return std::abs(distance(q, d2(p.c)) - static_cast<double>(p.r));
        const D2 d = p.b - p.a;
        const double len = norm(d);
        if (len <= 0.0)
            return distance(q, p.a);
        return std::abs(cross(d, q - p.a)) / len;
    }
    double deviation(int id, const Clipper2Lib::Point64& u, const Clipper2Lib::Point64& w) const
    {
        const D2 a{static_cast<double>(u.x), static_cast<double>(u.y)};
        const D2 b{static_cast<double>(w.x), static_cast<double>(w.y)};
        return std::max({distanceTo(id, a), distanceTo(id, b), distanceTo(id, (a + b) * 0.5)});
    }

    // Whether the polygon edges from..from+count-1 run counter-clockwise round c.
    static bool turnsCcw(Pt c, const Clipper2Lib::Path64& poly, std::size_t from, std::size_t count)
    {
        const std::size_t n = poly.size();
        double turn = 0.0;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& u = poly[(from + i) % n];
            const auto& w = poly[(from + i + 1) % n];
            turn += cross(D2{static_cast<double>(u.x - c.x), static_cast<double>(u.y - c.y)},
                          D2{static_cast<double>(w.x - c.x), static_cast<double>(w.y - c.y)});
        }
        return turn > 0.0;
    }

    D2 junction(int p1, int p2, D2 x) const
    {
        if (p1 < 0 || p2 < 0 || p1 == p2)
            return x;
        const Prim& a = raw_.prims[static_cast<std::size_t>(p1)];
        const Prim& b = raw_.prims[static_cast<std::size_t>(p2)];
        const double snap = 2.0 * sag_ + 4.0;
        // Consecutive on a raw curve (skipping degenerate connectors): the junction written down.
        const auto follows = [&](int from, int to) {
            int q = raw_.prims[static_cast<std::size_t>(from)].next;
            for (int hop = 0; hop < 3 && q >= 0; ++hop) {
                if (q == to)
                    return true;
                if (!raw_.prims[static_cast<std::size_t>(q)].degenerate)
                    return false;
                q = raw_.prims[static_cast<std::size_t>(q)].next;
            }
            return false;
        };
        if (follows(p1, p2) && distance(a.b, x) <= snap)
            return a.b;
        if (follows(p2, p1) && distance(b.b, x) <= snap)
            return b.b;
        const D2 j = intersect(a, b, x);
        return distance(j, x) <= 1e6 ? j : x;
    }

    // The intersection of two exact curves nearest x; where they miss, their closest approach.
    static D2 intersect(const Prim& a, const Prim& b, D2 x)
    {
        const auto nearest = [&](D2 p, D2 q) {
            return distance(p, x) <= distance(q, x) ? p : q;
        };
        if (!a.arc && !b.arc) {
            const D2 d1 = a.b - a.a, d2v = b.b - b.a;
            const double den = cross(d1, d2v);
            if (std::abs(den) <= 1e-12 * norm(d1) * norm(d2v))
                return x;
            const double t = cross(b.a - a.a, d2v) / den;
            return a.a + d1 * t;
        }
        if (a.arc && b.arc) {
            const D2 c1 = d2(a.c), c2 = d2(b.c);
            const D2 dc = c2 - c1;
            const double d = norm(dc);
            if (d <= 0.0)
                return x;
            const double r1 = static_cast<double>(a.r), r2 = static_cast<double>(b.r);
            const double along = (r1 * r1 - r2 * r2 + d * d) / (2.0 * d);
            const D2 u = dc * (1.0 / d);
            const D2 base = c1 + u * along;
            const double h2 = r1 * r1 - along * along;
            if (h2 <= 0.0)
                return base;
            const double h = dmath::sqrt(h2);
            return nearest(base + leftOf(u) * h, base + rightOf(u) * h);
        }
        const Prim& line = a.arc ? b : a;
        const Prim& arc = a.arc ? a : b;
        const D2 dir = unit(line.b - line.a);
        if (norm(dir) <= 0.0)
            return x;
        const D2 c = d2(arc.c);
        const D2 foot = line.a + dir * dot(c - line.a, dir);
        const double q = norm(foot - c);
        const double r = static_cast<double>(arc.r);
        const double h2 = (r - q) * (r + q);
        if (h2 <= 0.0)
            return foot;
        const double h = dmath::sqrt(h2);
        return nearest(foot + dir * h, foot - dir * h);
    }

    const Raw& raw_;
    double sag_;
    std::vector<std::array<int, 5>> crossings_;
    std::vector<int> candidates_;
};

// Runs Clipper2 on the raw curves (subject curves by group, see `isClip`) and rebuilds the result as regions; with
// `spans` also returns every rebuilt polygon with its primitives (offsetOpen).
struct ClipResult {
    Regions regions;
    std::vector<Rebuilt> polygons;
    std::size_t literalEdges = 0;
};
Result<ClipResult> run(const Raw& raw, Clipper2Lib::ClipType type, Clipper2Lib::FillRule fill, Coord sag,
                       const std::function<bool(int group)>& isClip, JobContext& ctx)
{
    Clipper2Lib::Paths64 subjects, clips;
    for (std::size_t i = 0; i < raw.curves.size(); ++i) {
        if (auto st = poll(ctx, i, 64); !st)
            return tl::unexpected(st.error());
        Clipper2Lib::Path64 poly = flatten(raw, raw.curves[i], sag);
        if (poly.size() < 2)
            continue;
        const int group = raw.prims[static_cast<std::size_t>(raw.curves[i].first)].group;
        (isClip && isClip(group) ? clips : subjects).push_back(std::move(poly));
    }
    Engine engine(raw, sag);
    Clipper2Lib::PolyTree64 tree;
    try {
        Clipper2Lib::Clipper64 clipper;
        clipper.SetZCallback(
            [&engine](const Clipper2Lib::Point64& e1bot, const Clipper2Lib::Point64& e1top,
                      const Clipper2Lib::Point64& e2bot, const Clipper2Lib::Point64& e2top,
                      Clipper2Lib::Point64& pt) { pt.z = engine.record(e1bot, e1top, e2bot, e2top, pt.z); });
        clipper.AddSubject(subjects);
        if (!clips.empty())
            clipper.AddClip(clips);
        if (!clipper.Execute(type, fill, tree))
            return tl::unexpected(
                Error{ErrorCode::Internal, "Clipper2 could not compute the 2D result.", {std::string(kWhere)}});
    } catch (const std::exception& e) {
        return tl::unexpected(
            Error{ErrorCode::Internal, std::string("Clipper2 failed: ") + e.what(), {std::string(kWhere)}});
    }
    if (auto st = ctx.cancel.check(); !st)
        return tl::unexpected(st.error());
    ClipResult out;
    std::size_t polled = 0;
    // Level 1: outers; level 2: their holes; level 3: islands (outers again); ...
    std::function<Status(const Clipper2Lib::PolyPath64&)> visitOuter =
        [&](const Clipper2Lib::PolyPath64& node) -> Status {
        if (auto st = poll(ctx, ++polled, 64); !st)
            return st;
        Rebuilt outer = engine.rebuild(node.Polygon());
        Region region;
        const bool keep = !outer.path.spans.empty() && std::abs(area(outer.path)) > 0.0;
        if (keep)
            region.outer = outer.path;
        out.polygons.push_back(std::move(outer));
        for (const auto& hole : node) {
            Rebuilt h = engine.rebuild(hole->Polygon());
            if (keep && !h.path.spans.empty() && std::abs(area(h.path)) > 0.0)
                region.holes.push_back(h.path);
            out.polygons.push_back(std::move(h));
            for (const auto& island : *hole)
                if (auto st = visitOuter(*island); !st)
                    return st;
        }
        if (keep)
            out.regions.push_back(std::move(region));
        return {};
    };
    for (const auto& child : tree)
        if (auto st = visitOuter(*child); !st)
            return tl::unexpected(st.error());
    out.regions = canonicalize(std::move(out.regions));
    out.literalEdges = engine.literalEdges;
    return out;
}

std::atomic<std::size_t>& literalCounter()
{
    static std::atomic<std::size_t> count{0};
    return count;
}

Status checkRegions(const Regions& rs)
{
    for (const Region& r : rs) {
        if (auto st = checkRange(r.outer); !st)
            return st;
        for (const Path& h : r.holes)
            if (auto st = checkRange(h); !st)
                return st;
    }
    return {};
}

Status checkDistance(Coord d)
{
    if (d < -kMaxCoord || d > kMaxCoord)
        return tl::unexpected(invalid("An offset distance is beyond 1 km."));
    return {};
}

Result<Regions> boolean(const Regions& a, const Regions& b, Clipper2Lib::ClipType type, JobContext& ctx)
{
    if (auto st = ctx.cancel.check(); !st)
        return tl::unexpected(st.error());
    if (auto st = checkRegions(a); !st)
        return tl::unexpected(st.error());
    if (auto st = checkRegions(b); !st)
        return tl::unexpected(st.error());
    Raw raw;
    for (const Region& r : canonicalize(a)) {
        addBoundary(raw, r.outer, 1);
        for (const Path& h : r.holes)
            addBoundary(raw, h, 1);
    }
    for (const Region& r : canonicalize(b)) {
        addBoundary(raw, r.outer, 2);
        for (const Path& h : r.holes)
            addBoundary(raw, h, 2);
    }
    auto result = run(raw, type, Clipper2Lib::FillRule::NonZero, kOffsetSag, [](int g) { return g == 2; }, ctx);
    if (!result)
        return tl::unexpected(result.error());
    literalCounter() += result->literalEdges;
    return std::move(result->regions);
}

} // namespace

std::size_t literalEdgeCount()
{
    return literalCounter().load();
}

Result<Regions> offset(const Regions& regions, Coord delta, JobContext& ctx, Coord sag)
{
    if (auto st = ctx.cancel.check(); !st)
        return tl::unexpected(st.error());
    if (auto st = checkRegions(regions); !st)
        return tl::unexpected(st.error());
    if (auto st = checkDistance(delta); !st)
        return tl::unexpected(st.error());
    if (delta == 0)
        return canonicalize(regions);
    Raw raw;
    std::size_t step = 0;
    for (const Region& r : canonicalize(regions)) {
        if (auto st = poll(ctx, ++step, 64); !st)
            return tl::unexpected(st.error());
        addOffsetBoundary(raw, r.outer, static_cast<double>(delta), 0);
        for (const Path& h : r.holes)
            addOffsetBoundary(raw, h, static_cast<double>(delta), 0);
    }
    auto result = run(raw, Clipper2Lib::ClipType::Union, Clipper2Lib::FillRule::Positive, sag, {}, ctx);
    if (!result)
        return tl::unexpected(result.error());
    literalCounter() += result->literalEdges;
    return std::move(result->regions);
}

Result<Paths> offsetOpen(const Path& open, Coord d, Side side, JobContext& ctx, Coord sag)
{
    if (auto st = ctx.cancel.check(); !st)
        return tl::unexpected(st.error());
    if (auto st = checkRange(open); !st)
        return tl::unexpected(st.error());
    if (d <= 0 || d > kMaxCoord)
        return tl::unexpected(invalid("The offset of an open path needs a distance above 0."));
    Raw raw;
    constexpr int kRight = 1, kLeft = 2, kCaps = 3;
    addSweepOutline(raw, canonicalize(open), static_cast<double>(d), kRight, kLeft, kCaps);
    auto result = run(raw, Clipper2Lib::ClipType::Union, Clipper2Lib::FillRule::Positive, sag, {}, ctx);
    if (!result)
        return tl::unexpected(result.error());
    literalCounter() += result->literalEdges;
    const int wanted = side == Side::Right ? kRight : kLeft;
    struct Run {
        Path path;
        int key;
    };
    std::vector<Run> runs;
    for (const Rebuilt& poly : result->polygons) {
        const std::size_t n = poly.path.spans.size();
        if (n == 0)
            continue;
        std::vector<int> groups(n);
        for (std::size_t i = 0; i < n; ++i) {
            const int prim = poly.prims[i];
            groups[i] = prim >= 0 ? raw.prims[static_cast<std::size_t>(prim)].group : (i > 0 ? groups[i - 1] : 0);
        }
        std::vector<Pt> starts(n);
        {
            Pt cur = poly.path.start;
            for (std::size_t i = 0; i < n; ++i) {
                starts[i] = cur;
                cur = poly.path.spans[i].end;
            }
        }
        std::size_t first = n;
        for (std::size_t i = 0; i < n; ++i)
            if (groups[i] != wanted) {
                first = i;
                break;
            }
        if (first == n) {
            Path closed = poly.path;
            runs.push_back(Run{side == Side::Left ? reverse(closed) : closed, 0});
            continue;
        }
        for (std::size_t i = 1; i <= n;) {
            const std::size_t k = (first + i) % n;
            if (groups[k] != wanted) {
                ++i;
                continue;
            }
            Run run;
            run.path.start = starts[k];
            run.key = side == Side::Right ? std::numeric_limits<int>::max() : std::numeric_limits<int>::min();
            while (i <= n && groups[(first + i) % n] == wanted) {
                const std::size_t j = (first + i) % n;
                run.path.spans.push_back(poly.path.spans[j]);
                if (poly.prims[j] >= 0)
                    run.key = side == Side::Right ? std::min(run.key, poly.prims[j]) : std::max(run.key, poly.prims[j]);
                ++i;
            }
            if (side == Side::Left)
                run.path = reverse(run.path);
            runs.push_back(std::move(run));
        }
    }
    std::stable_sort(runs.begin(), runs.end(),
                     [&](const Run& a, const Run& b) { return side == Side::Right ? a.key < b.key : a.key > b.key; });
    Paths out;
    for (Run& r : runs)
        out.push_back(canonicalize(std::move(r.path)));
    return out;
}

Result<Regions> sweptArea(std::span<const Path> paths, Coord r, JobContext& ctx, Coord sag)
{
    if (auto st = ctx.cancel.check(); !st)
        return tl::unexpected(st.error());
    if (r <= 0 || r > kMaxCoord)
        return tl::unexpected(invalid("A swept area needs a radius above 0."));
    Raw raw;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (auto st = poll(ctx, i, 64); !st)
            return tl::unexpected(st.error());
        if (auto st = checkRange(paths[i]); !st)
            return tl::unexpected(st.error());
        addSweepOutline(raw, canonicalize(paths[i]), static_cast<double>(r), 0, 0, 0);
    }
    auto result = run(raw, Clipper2Lib::ClipType::Union, Clipper2Lib::FillRule::Positive, sag, {}, ctx);
    if (!result)
        return tl::unexpected(result.error());
    literalCounter() += result->literalEdges;
    return std::move(result->regions);
}

Result<Regions> unite(const Regions& a, const Regions& b, JobContext& ctx)
{
    return boolean(a, b, Clipper2Lib::ClipType::Union, ctx);
}
Result<Regions> intersect(const Regions& a, const Regions& b, JobContext& ctx)
{
    return boolean(a, b, Clipper2Lib::ClipType::Intersection, ctx);
}
Result<Regions> subtract(const Regions& a, const Regions& b, JobContext& ctx)
{
    return boolean(a, b, Clipper2Lib::ClipType::Difference, ctx);
}

} // namespace ray::cam::geom2d
