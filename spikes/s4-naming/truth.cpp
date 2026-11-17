// Ground-truth oracle. Independent of naming.cpp: it never sees a name, a history or an index of a
// previous build. It knows only (a) the script and its current parameters, from which it derives, for
// every logical face of every feature, an analytic carrier surface and an analytic extent ("owner"),
// and (b) the geometry of a built shape (mesh nodes of faces, points on edges, vertices).
//
// Identity of a baseline element = the owner(s) whose carrier it lies on and whose extent it covers,
// plus, when a logical face is in several pieces, on which side of which other carriers the piece lies.
// After an edit the owners are recomputed from the new parameters and the elements of the new shape
// that satisfy the same description are the correct answer.
#include "s4.h"
#include <GeomAbs_SurfaceType.hxx>

namespace s4 {

// ---------------------------------------------------------------- carriers
double Carrier::dist(const gp_Pnt& x) const {
    gp_Vec d(p, x);
    double z = d.Dot(gp_Vec(a));
    switch (k) {
    case Plane: return z;
    case Cyl: return (d - z * gp_Vec(a)).Magnitude() - r;
    case Torus: { double rho = (d - z * gp_Vec(a)).Magnitude(); return std::hypot(rho - r, z) - r2; }
    case Cone: {
        double rho = (d - z * gp_Vec(a)).Magnitude();
        double dr = q[2] - q[0], dz = q[3] - q[1], len = std::hypot(dr, dz);
        return ((rho - q[0]) * dz - (z - q[1]) * dr) / len;
    }
    default: return 1e9;
    }
}
// Surface type must agree, and where the script knows it, so must the side the material is on.
// (Without the orientation test two different logical faces that lie back to back on one plane are confused;
// that happened in development: the lug's hidden side against the plate side.)
static bool typeMatches(const Carrier& c, const ElemSamples& es) {
    int t = es.type;
    switch (c.k) {
    case Carrier::Plane: return t == GeomAbs_Plane && (!c.oriented || es.nrm.Dot(c.a) > 0.99);
    case Carrier::Cyl: return t == GeomAbs_Cylinder && (!c.oriented || es.sigma == 0 || es.sigma == c.sigma);
    case Carrier::Torus: return t == GeomAbs_Torus;
    case Carrier::Cone: return t == GeomAbs_Cone;
    default: return false;
    }
}

// ---------------------------------------------------------------- 2D profile helpers
static double normAng(double x) { while (x < 0) x += 2 * M_PI; while (x >= 2 * M_PI) x -= 2 * M_PI; return x; }
static bool insideLoop(const Loop& l, double u, double v) {
    if (l.ents.size() == 1 && l.ents[0].k == 'C') return std::hypot(u - l.ents[0].cx, v - l.ents[0].cy) <= l.ents[0].r;
    bool in = false; // polygon of the chords
    for (const Ent& e : l.ents) {
        if ((e.y0 > v) != (e.y1 > v)) {
            double x = e.x0 + (v - e.y0) / (e.y1 - e.y0) * (e.x1 - e.x0);
            if (u < x) in = !in;
        }
    }
    if (in) return true;
    for (const Ent& e : l.ents) { // circular segments of ccw arcs bulge outwards of a ccw loop
        if (e.k != 'A') continue;
        double cr = (e.x1 - e.x0) * (v - e.y0) - (e.y1 - e.y0) * (u - e.x0);
        if (cr <= 0 && std::hypot(u - e.cx, v - e.cy) <= e.r) return true;
    }
    return false;
}
static double distEnt(const Ent& e, double u, double v) {
    if (e.k == 'L') {
        double dx = e.x1 - e.x0, dy = e.y1 - e.y0, len2 = dx * dx + dy * dy;
        double t = std::clamp(((u - e.x0) * dx + (v - e.y0) * dy) / len2, 0.0, 1.0);
        return std::hypot(u - (e.x0 + t * dx), v - (e.y0 + t * dy));
    }
    double rr = std::hypot(u - e.cx, v - e.cy);
    if (e.k == 'C') return std::abs(rr - e.r);
    double a0 = std::atan2(e.y0 - e.cy, e.x0 - e.cx), a1 = std::atan2(e.y1 - e.cy, e.x1 - e.cx), a = std::atan2(v - e.cy, u - e.cx);
    if (normAng(a - a0) <= normAng(a1 - a0)) return std::abs(rr - e.r);
    return std::min(std::hypot(u - e.x0, v - e.y0), std::hypot(u - e.x1, v - e.y1));
}
static double distLoop(const Loop& l, double u, double v) {
    double d = 1e9;
    for (const Ent& e : l.ents) d = std::min(d, distEnt(e, u, v));
    return d;
}

// ---------------------------------------------------------------- owners from the script
static bool blendCarrier(const Owner& A, const Owner& B, bool fillet, bool convex, double size, Carrier& out) {
    const Carrier *P = nullptr, *C = nullptr;
    if (A.c.k == Carrier::Plane && B.c.k == Carrier::Plane) {
        gp_Vec nA(A.c.a), nB(B.c.a);
        double c = nA.Dot(nB);
        if (std::abs(c) > 0.999) return false;
        gp_Vec dir = nA.Crossed(nB);
        double dA = nA.Dot(gp_Vec(A.c.p.XYZ())), dB = nB.Dot(gp_Vec(B.c.p.XYZ()));
        gp_Vec e0 = (dA * nB.Crossed(dir) + dB * dir.Crossed(nA)) / dir.SquareMagnitude();
        double s = convex ? -1.0 : 1.0;
        if (fillet) {
            gp_Vec ax = e0 + s * size * (nA + nB) / (1 + c);
            out.k = Carrier::Cyl; out.p = gp_Pnt(ax.XYZ()); out.a = gp_Dir(dir); out.r = size; out.sigma = convex ? +1 : -1; out.oriented = true;
        } else {
            double sn = std::sqrt(1 - c * c);
            gp_Vec pA = e0 + s * size * (nB - c * nA) / sn, pB = e0 + s * size * (nA - c * nB) / sn;
            out.k = Carrier::Plane; out.p = gp_Pnt(pA.XYZ()); out.a = gp_Dir(dir.Crossed(pA - pB));
        }
        return true;
    }
    if (A.c.k == Carrier::Plane && B.c.k == Carrier::Cyl) { P = &A.c; C = &B.c; }
    else if (A.c.k == Carrier::Cyl && B.c.k == Carrier::Plane) { P = &B.c; C = &A.c; }
    else return false;
    double al = gp_Vec(P->a).Dot(gp_Vec(C->a));
    if (std::abs(al) < 0.999) return false;
    double sP = al > 0 ? 1.0 : -1.0;
    double z0 = gp_Vec(C->p, P->p).Dot(gp_Vec(C->a));
    double R = C->r, sc = C->sigma;
    out.p = C->p; out.a = C->a;
    if (fillet) {
        double zc = convex ? z0 - sP * size : z0 + sP * size;
        double rho0 = convex ? R - sc * size : R + sc * size;
        out.k = Carrier::Torus; out.p = gp_Pnt(C->p.XYZ() + zc * C->a.XYZ()); out.r = rho0; out.r2 = size;
    } else {
        out.k = Carrier::Cone;
        if (convex) { out.q[0] = R; out.q[1] = z0 - sP * size; out.q[2] = R - sc * size; out.q[3] = z0; }
        else { out.q[0] = R; out.q[1] = z0 + sP * size; out.q[2] = R + sc * size; out.q[3] = z0; }
    }
    return true;
}

Owners buildOwners(const Script& script) {
    Owners O;
    auto add = [&](Owner o) { O.ix[o.key] = (int)O.v.size(); O.v.push_back(std::move(o)); };
    for (const Feature& f : script) {
        if (f.suppressed) continue;
        if (f.type == FType::Sweep) {
            const Frame fr = f.frame;
            const double sg = f.op == BOp::Cut ? -1.0 : 1.0, D = f.dist;
            const bool cut = f.op == BOp::Cut, hole = f.role == "hole";
            auto local = [fr, sg](const gp_Pnt& p, double& u, double& v, double& h) {
                gp_Vec d(fr.o, p); u = d.Dot(gp_Vec(fr.xd)); v = d.Dot(gp_Vec(fr.yd())); h = sg * d.Dot(gp_Vec(fr.n));
            };
            for (const Loop& l : f.loops) {
                for (const Ent& e : l.ents) {
                    Owner o; o.key = f.id + "/s" + std::to_string(e.tag);
                    o.cat = hole ? "hole wall" : cut ? "cut wall" : "boss/base side";
                    if (e.k == 'L') {
                        double len = std::hypot(e.x1 - e.x0, e.y1 - e.y0), tx = (e.x1 - e.x0) / len, ty = (e.y1 - e.y0) / len;
                        double ox = cut ? -ty : ty, oy = cut ? tx : -tx;
                        o.c.oriented = true;
                        o.c.k = Carrier::Plane; o.c.p = fr.at(e.x0, e.y0);
                        o.c.a = gp_Dir(ox * fr.xd.XYZ() + oy * fr.yd().XYZ());
                        o.in = [=](const gp_Pnt& p, double m) {
                            double u, v, h; local(p, u, v, h);
                            double t = (u - e.x0) * tx + (v - e.y0) * ty;
                            return t >= -m && t <= len + m && h >= -m && h <= D + m;
                        };
                    } else {
                        o.c.oriented = true;
                        o.c.k = Carrier::Cyl; o.c.p = fr.at(e.cx, e.cy); o.c.a = fr.n; o.c.r = e.r; o.c.sigma = cut ? -1 : +1;
                        double a0 = std::atan2(e.y0 - e.cy, e.x0 - e.cx), span = e.k == 'C' ? 2 * M_PI : normAng(std::atan2(e.y1 - e.cy, e.x1 - e.cx) - a0);
                        o.in = [=](const gp_Pnt& p, double m) {
                            double u, v, h; local(p, u, v, h);
                            if (h < -m || h > D + m) return false;
                            if (e.k == 'C') return true;
                            double a = normAng(std::atan2(v - e.cy, u - e.cx) - a0), ma = m / e.r;
                            return a <= span + ma || a >= 2 * M_PI - ma;
                        };
                    }
                    add(o);
                }
                for (int cap = 0; cap < 2; ++cap) {
                    Owner o; o.key = f.id + "/c" + std::to_string(cap) + "/" + std::to_string(l.id);
                    o.cat = cap == 0 ? "bottom face" : hole ? "hole floor" : cut ? "cut floor" : "top face";
                    o.c.k = Carrier::Plane; o.c.oriented = true;
                    o.c.p = fr.at(0, 0, cap == 0 ? 0.0 : sg * D);
                    gp_Dir dir = f.dir();
                    o.c.a = (cap == 0 || cut) ? dir.Reversed() : dir;
                    Loop lp = l;
                    o.in = [=](const gp_Pnt& p, double m) {
                        double u, v, h; local(p, u, v, h);
                        return insideLoop(lp, u, v) || distLoop(lp, u, v) <= m;
                    };
                    add(o);
                }
            }
        } else {
            const bool fillet = f.type == FType::Fillet;
            for (size_t k = 0; k < f.targets.size(); ++k) {
                const Owner *A = O.get(f.targets[k].a), *B = O.get(f.targets[k].b);
                if (!A || !B) continue;
                Owner o; o.key = f.id + "/b" + std::to_string(k);
                o.cat = fillet ? "fillet face" : "chamfer face";
                if (!blendCarrier(*A, *B, fillet, f.targets[k].convex, f.size, o.c)) continue;
                Owner a = *A, b = *B; double rho = f.size;
                o.in = [=](const gp_Pnt& p, double m) {
                    return std::abs(a.c.dist(p)) <= rho + m + 1e-3 && std::abs(b.c.dist(p)) <= rho + m + 1e-3 &&
                           a.in(p, m + rho + 0.01) && b.in(p, m + rho + 0.01);
                };
                add(o);
            }
        }
    }
    return O;
}

// ---------------------------------------------------------------- evaluation
namespace {
struct Tol { double node, sample, margin, thr; };
Tol tolFor(Kind k) {
    if (k == KFace) return {1e-4, 0.06, 1e-4, 0.02};
    if (k == KEdge) return {1e-3, 1e-3, 2e-3, 0.05};
    return {1e-3, 1e-3, 2e-3, 0.5};
}
using SepList = std::vector<std::pair<const Carrier*, int>>;
bool sepsOk(const SepList& seps, const gp_Pnt& p) {
    for (auto& [c, s] : seps) if (s * c->dist(p) < -1e-3) return false;
    return true;
}
} // namespace

TruthEval evalTruth(const TruthDesc& d, const Owners& ow, const ShapeIdx& idx, const Samples& smp) {
    TruthEval ev;
    const Tol T = tolFor(d.kind);
    struct C { std::vector<const Owner*> alts; SepList seps; };
    std::vector<C> cons;
    // A separator follows its owner when the owner still exists; if the owner is gone (feature deleted or
    // suppressed) the baseline geometry of the separator is used, so "which piece" stays a geometric question.
    auto conv = [&](const std::vector<Sep>& in, SepList& out) {
        for (const Sep& s : in) { const Owner* o = ow.get(s.owner); if (o) out.push_back({&o->c, s.side}); else { out.push_back({&s.frozen, s.side}); ev.droppedSep = true; } }
    };
    for (const Constraint& c : d.cons) {
        C x;
        for (const auto& a : c.alts) if (const Owner* o = ow.get(a)) x.alts.push_back(o);
        if (x.alts.empty()) return ev; // the logical face no longer exists in the script
        conv(c.seps, x.seps);
        cons.push_back(std::move(x));
    }
    SepList dseps;
    conv(d.seps, dseps);
    const auto& E = smp.e[d.kind];
    for (int x = 0; x < (int)E.size(); ++x) {
        const ElemSamples& es = E[x];
        if (es.nodes.empty() || es.pts.empty()) continue;
        bool carr = true;
        for (const C& c : cons) {
            bool anyAlt = false;
            for (const Owner* o : c.alts) {
                if (d.kind == KFace && !typeMatches(o->c, es)) continue;
                bool all = true;
                for (const gp_Pnt& p : es.nodes) if (std::abs(o->c.dist(p)) > T.node) { all = false; break; }
                if (all) { anyAlt = true; break; }
            }
            if (!anyAlt) { carr = false; break; }
        }
        if (!carr) continue;
        double okw = 0, W = 0;
        for (size_t j = 0; j < es.pts.size(); ++j) {
            const gp_Pnt& p = es.pts[j];
            W += es.w[j];
            bool ok = sepsOk(dseps, p);
            for (size_t ci = 0; ok && ci < cons.size(); ++ci) {
                bool any = false;
                if (sepsOk(cons[ci].seps, p))
                    for (const Owner* o : cons[ci].alts)
                        if (std::abs(o->c.dist(p)) <= T.sample && o->in(p, T.margin)) { any = true; break; }
                ok = any;
            }
            if (ok) okw += es.w[j];
        }
        double frac = W > 0 ? okw / W : 0;
        if (frac >= T.thr) { ev.elems.push_back(x + 1); if (frac < 0.98) ev.merged = true; }
    }
    return ev;
}

// Find carriers that separate element `self` from the other elements that satisfy `d`.
static bool findSeps(TruthDesc& d, int self, std::vector<Sep>& where, const Owners& ow, const ShapeIdx& idx, const Samples& smp) {
    for (int iter = 0; iter < 6; ++iter) {
        TruthEval ev = evalTruth(d, ow, idx, smp);
        if (ev.elems.size() == 1 && ev.elems[0] == self) return true;
        if (std::find(ev.elems.begin(), ev.elems.end(), self) == ev.elems.end()) return false;
        int other = 0;
        for (int x : ev.elems) if (x != self) { other = x; break; }
        const ElemSamples &S = smp.e[d.kind][self - 1], &Oth = smp.e[d.kind][other - 1];
        bool added = false;
        for (const Owner& o : ow.v) {
            if (o.c.k != Carrier::Plane && o.c.k != Carrier::Cyl) continue;
            double smin = 1e9, smax = -1e9, omin = 1e9, omax = -1e9, touch = 1e9;
            // The separator must bound the piece: some boundary point of the piece lies on the carrier AND inside
            // the owner's extent. (Without the extent test a coincident but unrelated carrier can be chosen; that
            // happened in development: a hole floor plane at the same height as a groove wall.)
            for (const gp_Pnt& p : S.nodes) { double x = o.c.dist(p); smin = std::min(smin, x); smax = std::max(smax, x); if (std::abs(x) < touch && o.in(p, 1e-2)) touch = std::abs(x); }
            for (const gp_Pnt& p : Oth.nodes) { double x = o.c.dist(p); omin = std::min(omin, x); omax = std::max(omax, x); }
            if (touch > 1e-3) continue;
            int side = 0;
            if (smin >= -1e-3 && smax > 0.05 && omax <= 1e-3 && omin < -0.05) side = +1;
            if (smax <= 1e-3 && smin < -0.05 && omin >= -1e-3 && omax > 0.05) side = -1;
            if (!side) continue;
            where.push_back({o.key, side, o.c});
            added = true;
            break;
        }
        if (!added) return false;
    }
    return false;
}

TruthBuilder::TruthBuilder(const Owners& o, const ShapeIdx& i, const Samples& s) : ow(o), idx(i), smp(s) {
    faceDesc.resize(idx.count(KFace));
    for (int f = 1; f <= idx.count(KFace); ++f) faceDesc[f - 1] = describe({KFace, f});
}

TruthDesc TruthBuilder::describe(SubId s) {
    TruthDesc d; d.kind = s.kind;
    if (s.kind == KFace) {
        const ElemSamples& es = smp.e[KFace][s.index - 1];
        if (es.nodes.empty() || es.pts.empty()) { d.why = "no mesh"; return d; }
        Constraint c; double total = 0; double W = 0;
        for (double w : es.w) W += w;
        for (const Owner& o : ow.v) {
            bool all = true;
            for (const gp_Pnt& p : es.nodes) if (std::abs(o.c.dist(p)) > 1e-4) { all = false; break; }
            if (!all) continue;
            if (!typeMatches(o.c, es)) { typeMismatch++; continue; }
            double okw = 0;
            for (size_t j = 0; j < es.pts.size(); ++j) if (o.in(es.pts[j], 1e-4)) okw += es.w[j];
            if (okw / W >= 0.02) { c.alts.push_back(o.key); total += okw / W; if (d.cat.empty()) d.cat = o.cat; }
        }
        if (c.alts.empty()) { d.why = "no owner"; return d; }
        if (total < 0.98) { d.why = "owners cover only part of the face"; return d; }
        d.cons.push_back(c);
        if (!findSeps(d, s.index, d.cons[0].seps, ow, idx, smp)) { d.why = "not unique at baseline"; return d; }
        TruthEval ev = evalTruth(d, ow, idx, smp);
        if (ev.merged) { d.why = "baseline self-check not full"; return d; }
        d.ok = true;
        return d;
    }
    std::vector<int> fs = s.kind == KEdge ? idx.facesOfEdge(s.index) : idx.facesOfVertex(s.index);
    if (fs.size() < 2) { d.why = "fewer than two faces (seam)"; return d; }
    for (int f : fs) {
        if (!faceDesc[f - 1].ok) { d.why = "adjacent face has no truth"; return d; }
        d.cons.push_back(faceDesc[f - 1].cons[0]);
    }
    d.cat = s.kind == KEdge ? "edge" : "vertex";
    if (!findSeps(d, s.index, d.seps, ow, idx, smp)) {
        d.seps.clear();
        TruthEval ev = evalTruth(d, ow, idx, smp);
        if (std::find(ev.elems.begin(), ev.elems.end(), s.index) == ev.elems.end()) { d.why = "not found at baseline"; return d; }
        d.weak = true; d.cat += " (weak oracle)";
    } else if (s.kind == KVertex && fs.size() < 3) {
        // A vertex on only two faces (the seam vertex of a hole rim) is just "some point of that curve": the
        // oracle cannot tell it from another vertex that later appears on the same curve.
        d.weak = true; d.cat += " (weak oracle)";
    }
    d.ok = true;
    return d;
}

std::vector<std::pair<int, TruthDesc>> pickTarget(const Target& t, const Owners& ow, const ShapeIdx& idx, const Samples& smp) {
    std::vector<std::pair<int, TruthDesc>> out;
    TruthDesc d; d.kind = KEdge; d.cat = "feature pick";
    d.cons.push_back(Constraint{{t.a}, {}});
    d.cons.push_back(Constraint{{t.b}, {}});
    if (!ow.get(t.a) || !ow.get(t.b)) return out;
    TruthEval ev = evalTruth(d, ow, idx, smp);
    for (int e : ev.elems) {
        TruthDesc x = d;
        x.ok = findSeps(x, e, x.seps, ow, idx, smp);
        if (!x.ok) x.why = "pick not unique";
        out.push_back({e, x});
    }
    return out;
}

} // namespace s4
