// Spike S4 driver: build the 40-feature part, record references, run the edit scenarios,
// resolve every reference and judge each resolution against the independent oracle (truth.cpp).
#include "s4.h"
#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Version.hxx>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>

using namespace s4;

static double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---------------------------------------------------------------- verdicts
enum Verdict { VCorrect, VBrokenGone, VMissed, VWrongOther, VWrongIncomplete, VWrongGhost, VSilentMerge, VCount };
static const char* verdictName(int v) {
    static const char* n[] = {"correct", "broken-and-really-gone", "missed (broken but exists)", "WRONG different element",
                              "WRONG incomplete set", "WRONG ghost (should be gone)", "WRONG unflagged merge"};
    return n[v];
}
static bool isWrong(int v) { return v >= VWrongOther; }
static Verdict judge(const Resolved& r, const TruthEval& te) {
    std::vector<int> X;
    for (SubId s : r.elems) X.push_back(s.index);
    std::sort(X.begin(), X.end());
    std::vector<int> E = te.elems;
    std::sort(E.begin(), E.end());
    if (r.state == RState::Broken) return E.empty() ? VBrokenGone : VMissed;
    if (X == E) {
        if (r.state == RState::Exact && te.merged) return VSilentMerge;
        return VCorrect;
    }
    if (E.empty()) return VWrongGhost;
    if (std::includes(E.begin(), E.end(), X.begin(), X.end())) return VWrongIncomplete;
    return VWrongOther;
}

// Weak refs: the oracle only knows the set of pieces of the logical element (E), not which piece was picked.
// `cong` is the element congruent in place with the picked one, or 0.
static Verdict judgeWeak(const Resolved& r, const TruthEval& te, int cong) {
    std::vector<int> X;
    for (SubId s : r.elems) X.push_back(s.index);
    std::sort(X.begin(), X.end());
    std::vector<int> E = te.elems;
    std::sort(E.begin(), E.end());
    if (r.state == RState::Broken) return E.empty() ? VBrokenGone : VMissed;
    if (E.empty()) return VWrongGhost;
    if (!std::includes(E.begin(), E.end(), X.begin(), X.end())) return VWrongOther;
    if (cong && (r.state == RState::Exact || r.state == RState::Repaired) && !(X.size() == 1 && X[0] == cong)) return VWrongOther;
    return VCorrect;
}

// ---------------------------------------------------------------- build pipeline
struct Pick { int target; TopoRef ref; TruthDesc truth; };
using FeatRefs = std::map<std::string, std::vector<Pick>>;
struct OpDiag { // per OCCT operation
    std::string feature, op;
    int nIn[3]{}, vanished[3]{}, deleted[3]{}, nRes[3]{};
    PropStats ps;
    int surfMismatch{0};
    double opMs{}, propUs{};
};
struct PickOutcome { std::string feature; int target; Resolved r; TruthEval te; Verdict v; TopoRef ref; };
struct Build {
    std::shared_ptr<ShapeIdx> idx;
    ElementMap map;
    NameTable nt;
    Lineage lin;
    std::vector<OpDiag> ops;
    std::vector<PickOutcome> picks;
    std::vector<std::string> failed, invalid;
    int builtFeatures{0};
    double propUs{0}, opMs{0};
    int unverifiablePicks{0};
};

static int surfMismatches(const ShapeHistory& h, const std::vector<const ShapeIdx*>& ins, const ShapeIdx& res) {
    int n = 0;
    for (const HistoryEntry& e : h.entries) {
        if (e.rel != HistoryEntry::Modified || e.kept || e.src.kind == KVertex) continue;
        const Fingerprint& a = ins[e.input]->fingerprint(e.src);
        for (SubId r : e.res) {
            if (r.kind != e.src.kind) { n++; continue; }
            const Fingerprint& b = res.fingerprint(r);
            bool same = std::llabs(a.qAxisX - b.qAxisX) <= 10 && std::llabs(a.qAxisY - b.qAxisY) <= 10 && std::llabs(a.qAxisZ - b.qAxisZ) <= 10;
            bool flip = std::llabs(a.qAxisX + b.qAxisX) <= 10 && std::llabs(a.qAxisY + b.qAxisY) <= 10 && std::llabs(a.qAxisZ + b.qAxisZ) <= 10;
            if (a.type != b.type || a.qRadius != b.qRadius || !(same || flip)) n++;
        }
    }
    return n;
}

static Build build(const Script& sc, const Rules& R, bool author, FeatRefs& refs, const Owners& ow) {
    Build B;
    bool have = false;
    ElementMap cur;
    auto diag = [&](const std::string& fid, const std::string& op, const OpOut& o, const std::vector<const ShapeIdx*>& ins, const PropStats& ps, double us) {
        OpDiag d; d.feature = fid; d.op = op; d.ps = ps; d.opMs = o.ms; d.propUs = us;
        for (int k = 0; k < 3; ++k) { d.nIn[k] = o.hist.nIn[k]; d.vanished[k] = o.hist.vanished[k]; d.deleted[k] = o.hist.deletedOk[k]; d.nRes[k] = o.result->count((Kind)k); }
        if (!ins.empty()) d.surfMismatch = surfMismatches(o.hist, ins, *o.result);
        B.ops.push_back(d); B.propUs += us; B.opMs += o.ms;
        if (!o.valid) B.invalid.push_back(fid);
    };
    for (const Feature& f : sc) {
        if (f.suppressed) continue;
        if (f.type == FType::Sweep) {
            OpOut tool = opSweep(f);
            if (!tool.ok) { B.failed.push_back(f.id + ": " + tool.err); continue; }
            PropStats ps; double t0 = nowMs();
            NamingInput ni{f.id, f.role, {}, nullptr, tool.result, &tool.origins};
            ElementMap tm = propagate(ni, B.nt, B.lin, R, ps);
            diag(f.id, "prism", tool, {}, ps, (nowMs() - t0) * 1000);
            if (f.op == BOp::New || !have) { cur = std::move(tm); have = true; B.builtFeatures++; continue; }
            OpOut b = opBoolean(f.op, *cur.idx, *tool.result, R.unify);
            if (!b.ok) { B.failed.push_back(f.id + ": " + b.err); continue; }
            PropStats ps2; t0 = nowMs();
            NamingInput nb{f.id, f.role, {&cur, &tm}, &b.hist, b.result, nullptr};
            ElementMap nm = propagate(nb, B.nt, B.lin, R, ps2);
            diag(f.id, f.op == BOp::Cut ? "cut" : "fuse", b, {cur.idx.get(), tm.idx.get()}, ps2, (nowMs() - t0) * 1000);
            cur = std::move(nm); B.builtFeatures++;
        } else {
            if (!have) continue;
            std::set<int> edges;
            auto smp = sampleShape(*cur.idx, false);
            if (author) {
                auto& v = refs[f.id];
                for (int k = 0; k < (int)f.targets.size(); ++k) {
                    if (!f.targets[k].picked) continue;
                    for (auto& [e, desc] : pickTarget(f.targets[k], ow, *cur.idx, *smp)) {
                        v.push_back({k, makeRef(cur, B.nt, {KEdge, e}), desc});
                        edges.insert(e);
                        if (!desc.ok) B.unverifiablePicks++;
                    }
                }
            } else {
                auto it = refs.find(f.id);
                if (it != refs.end())
                    for (const Pick& p : it->second) {
                        Resolved r = resolve(p.ref, cur, B.nt, B.lin, R);
                        PickOutcome po{f.id, p.target, r, {}, VCorrect, p.ref};
                        if (p.truth.ok) { po.te = evalTruth(p.truth, ow, *cur.idx, *smp); po.v = judge(r, po.te); B.picks.push_back(po); }
                        for (SubId s : r.elems) edges.insert(s.index);
                    }
            }
            if (edges.empty()) { B.failed.push_back(f.id + ": no resolvable edge reference"); continue; }
            OpOut o = opBlend(f.type == FType::Fillet, *cur.idx, {edges.begin(), edges.end()}, f.size);
            if (!o.ok) { B.failed.push_back(f.id + ": " + o.err); continue; }
            PropStats ps; double t0 = nowMs();
            NamingInput ni{f.id, f.role, {&cur}, &o.hist, o.result, nullptr};
            ElementMap nm = propagate(ni, B.nt, B.lin, R, ps);
            diag(f.id, f.type == FType::Fillet ? "fillet" : "chamfer", o, {cur.idx.get()}, ps, (nowMs() - t0) * 1000);
            cur = std::move(nm); B.builtFeatures++;
        }
    }
    B.idx = cur.idx;
    B.map = std::move(cur);
    return B;
}

// ---------------------------------------------------------------- references on the baseline
struct Ref { SubId base; TopoRef ref; TruthDesc truth; std::string cat; };

static std::string nameStr(const NameTable& nt, NameId id, int depth = 2) {
    auto it = nt.t.find(id);
    if (it == nt.t.end()) { char b[32]; std::snprintf(b, sizeof b, "#%08llx", (unsigned long long)(id & 0xffffffff)); return b; }
    const TopoName& n = it->second;
    std::string s = "(" + n.origin + "," + n.role + ",[";
    for (size_t i = 0; i < n.parents.size(); ++i) { if (i) s += ","; s += depth > 0 ? nameStr(nt, n.parents[i], depth - 1) : "..."; }
    s += "]," + std::to_string(n.ordinal) + "/" + std::to_string(n.group) + ")";
    return s;
}
static std::string fpStr(const Fingerprint& f) {
    char b[160];
    std::snprintf(b, sizeof b, "type=%d meas=%.4f c=(%.3f,%.3f,%.3f) r=%.3f nb=%d", f.type, f.qArea / 1e4, f.qCx / 1e4, f.qCy / 1e4, f.qCz / 1e4, f.qRadius / 1e4, (int)f.neighbourCount);
    return b;
}

// ---------------------------------------------------------------- scenarios
struct Scenario {
    std::string id, desc;
    std::function<void(Params&)> p;
    std::function<void(Script&)> s;
};
static void del(Script& s, const std::string& id) { s.erase(std::remove_if(s.begin(), s.end(), [&](const Feature& f) { return f.id == id; }), s.end()); }
static void suppress(Script& s, const std::string& id) { for (auto& f : s) if (f.id == id) f.suppressed = true; }
static void moveAfter(Script& s, const std::string& id, const std::string& after) {
    auto it = std::find_if(s.begin(), s.end(), [&](const Feature& f) { return f.id == id; });
    Feature f = *it; s.erase(it);
    auto at = std::find_if(s.begin(), s.end(), [&](const Feature& x) { return x.id == after; });
    s.insert(at + 1, f);
}
static void swapF(Script& s, const std::string& a, const std::string& b) {
    auto ia = std::find_if(s.begin(), s.end(), [&](const Feature& f) { return f.id == a; });
    auto ib = std::find_if(s.begin(), s.end(), [&](const Feature& f) { return f.id == b; });
    std::iter_swap(ia, ib);
}

static std::vector<Scenario> scenarios() {
    using P = Params&; using S = Script&;
    return {
        {"N00", "no-op rebuild (determinism check)", nullptr, nullptr},
        // --- PLAN 3.3 acceptance: a dimension change on each of the first 10 features
        {"D01", "F01 base: L 160->172", [](P p) { p["L"] = 172; }, nullptr},
        {"D01h", "F01 base: H 30->34", [](P p) { p["H"] = 34; }, nullptr},
        {"D02", "F02 corner fillet: r 6->9", [](P p) { p["F02.r"] = 9; }, nullptr},
        {"D03", "F03 boss: x1 62->66, h 14->17", [](P p) { p["F03.x1"] = 66; p["F03.h"] = 17; }, nullptr},
        {"D04", "F04 round boss: r 20->21, h 10->12", [](P p) { p["F04.r"] = 21; p["F04.h"] = 12; }, nullptr},
        {"D05", "F05 step: wall y 88->86.5, depth 8->10", [](P p) { p["F05.y0"] = 86.5; p["F05.d"] = 10; }, nullptr},
        {"D06", "F06 pocket A: x0 72->69, depth 14->17", [](P p) { p["F06.x0"] = 69; p["F06.d"] = 17; }, nullptr},
        {"D07", "F07 pocket B: x1 54->52, y0 30->32, depth 10->12", [](P p) { p["F07.x1"] = 52; p["F07.y0"] = 32; p["F07.d"] = 12; }, nullptr},
        {"D08", "F08 pocket B corner fillet: r 4->6", [](P p) { p["F08.r"] = 6; }, nullptr},
        {"D09", "F09 slot: r 5->6, x1 132->136", [](P p) { p["F09.r"] = 6; p["F09.x1"] = 136; }, nullptr},
        {"D35", "F35 bottom chamfer: d 1->1.5", [](P p) { p["F35.d"] = 1.5; }, nullptr},
        {"D10", "F10 corner holes: dia 9->9.6, hole 1 moved (10,10)->(12,11)", [](P p) { p["F10.d"] = 9.6; p["F10.h1x"] = 12; p["F10.h1y"] = 11; }, nullptr},
        {"D10t", "F10 corner holes: dia 9->10, two holes become tangent to the y=W side (degenerate)", [](P p) { p["F10.d"] = 10; }, nullptr},
        {"B36", "F10 holes 3 and 4 moved so they break out of the y=W side", [](P p) { p["F10.h4y"] = 97.5; }, nullptr},
        // --- task list
        {"M12", "move a hole: F27 hole 2 x 80->86, F14 hole 1 x 16->17", [](P p) { p["F27.x2"] = 86; p["F14.x1"] = 17; }, nullptr},
        {"R13", "fillet radius: F20 pocket floor 2->3", [](P p) { p["F20.r"] = 3; }, nullptr},
        {"R13b", "fillet radius: F15 boss corners 5->7", [](P p) { p["F15.r"] = 7; }, nullptr},
        {"A14", "add feature early: X1 through notch after F01 (splits the x=L side)", [](P p) { p["X1.on"] = 1; }, nullptr},
        {"A15", "add feature early: X2 blind pocket after F01", [](P p) { p["X2.on"] = 1; }, nullptr},
        {"A16", "add feature at the end: Y1 groove across the boss top (splits faces and edges)", [](P p) { p["Y1.on"] = 1; }, nullptr},
        {"X16", "delete F09 slot", nullptr, [](S s) { del(s, "F09"); }},
        {"X17", "delete F24 island", nullptr, [](S s) { del(s, "F24"); }},
        {"X18", "delete F21 notch (two pieces of a face re-join)", nullptr, [](S s) { del(s, "F21"); }},
        {"O19", "reorder: swap F06 pocket A and F07 pocket B", nullptr, [](S s) { swapF(s, "F06", "F07"); }},
        {"O20", "reorder: swap F09 slot and F10 holes", nullptr, [](S s) { swapF(s, "F09", "F10"); }},
        {"O21", "reorder: move F31 dowel holes to just after F01", nullptr, [](S s) { moveAfter(s, "F31", "F01"); }},
        {"O22", "reorder: swap F03 boss and F04 round boss", nullptr, [](S s) { swapF(s, "F03", "F04"); }},
        {"O23", "reorder: move F21 notch (a splitting cut) to just after F02", nullptr, [](S s) { moveAfter(s, "F21", "F02"); }},
        {"P23", "sketch change, face splits: F09 slot moved to break through the y=0 side", [](P p) { p["F09.cy"] = 4; }, nullptr},
        {"P24", "sketch change, face splits: F29 pocket C extended through the step wall", [](P p) { p["F29.y1"] = 90.5; p["F29.d"] = 9; }, nullptr},
        {"U25", "faces un-merge: F38 channel depth 6->5, its floor leaves the pocket C floor", [](P p) { p["F38.d"] = 5; }, nullptr},
        {"G25", "faces merge: F06 pocket A depth 14->6, its floor becomes coplanar with channel and pocket C floors", [](P p) { p["F06.d"] = 6; }, nullptr},
        {"G26", "sketch change, faces merge: F29 vertex moved so two sides become collinear", [](P p) { p["F29.vy"] = 84; }, nullptr},
        {"G27", "faces merge: F32 lug lowered 3 mm, its bottom becomes flush with the plate bottom", [](P p) { p["F32.z0"] = 0; }, nullptr},
        {"X19", "delete F38 channel (edge pieces re-join, merged floor un-merges)", nullptr, [](S s) { del(s, "F38"); }},
        {"X20", "delete F40 groove (two pieces of the x=0 side re-join)", nullptr, [](S s) { del(s, "F40"); }},
        {"Z28", "suppress F15 boss corner fillets", nullptr, [](S s) { suppress(s, "F15"); }},
        {"Z29", "suppress F02 plate corner fillets", nullptr, [](S s) { suppress(s, "F02"); }},
        {"Z30", "suppress F06 pocket A", nullptr, [](S s) { suppress(s, "F06"); }},
        {"K31", "add a sketch line: F03 boss corner cut", [](P p) { p["F03.cut"] = 6; }, nullptr},
        {"K32", "add a sketch line: F01 base corner cut", [](P p) { p["F01.cut"] = 8; }, nullptr},
        {"K33", "add a sketch line: F07 pocket B corner cut", [](P p) { p["F07.cut"] = 5; }, nullptr},
        {"K34", "add sketch lines: F05 step wall gets a jog", [](P p) { p["F05.jog"] = 1; }, nullptr},
        {"V34", "ordinal attack: add X3 early; centroid order of the two pieces of the x=0 side flips", [](P p) { p["X3.on"] = 1; }, nullptr},
        {"V35", "ordinal attack 2: F40 groove moved up (z 10..14 -> 17..21)", [](P p) { p["F40.z0"] = 17; p["F40.z1"] = 21; }, nullptr},
        {"C35", "combined: L 170, F06 depth 16, hole moved, F20 r 2.5, F03 h 16, F27 x2 84",
         [](P p) { p["L"] = 170; p["F06.d"] = 16; p["F10.h1x"] = 11.5; p["F20.r"] = 2.5; p["F03.h"] = 16; p["F27.x2"] = 84; }, nullptr},
    };
}

// ---------------------------------------------------------------- reporting helpers
struct Tally {
    int n{0}, st[5]{}, v[VCount]{};
    void add(RState s, Verdict vv) { n++; st[(int)s]++; v[vv]++; }
    int wrong() const { return v[VWrongOther] + v[VWrongIncomplete] + v[VWrongGhost] + v[VSilentMerge]; }
    void merge(const Tally& o) { n += o.n; for (int i = 0; i < 5; ++i) st[i] += o.st[i]; for (int i = 0; i < VCount; ++i) v[i] += o.v[i]; }
};
static std::string pct(int a, int n) { char b[32]; std::snprintf(b, sizeof b, "%.1f", n ? 100.0 * a / n : 0.0); return b; }

struct RunOut { std::string md; int wrong{0}; };

static RunOut runAll(const Rules& R, bool verbose) {
    std::ostringstream md;
    char buf[4096];
    auto P = [&](const char* fmt, auto... a) { std::snprintf(buf, sizeof buf, fmt, a...); md << buf; std::fputs(buf, stdout); };

    P("\n## Rule set %s\n\n", R.label.c_str());
    P("A1 groupInHash=%d, A2 hintOnAmbiguous=%d, A3 inferEdgeHistory=%d, A4 unsplitFallback=%d, A5 familyFallback=%d, A7 topoDisc=%d, A8 repairNeedsNeighbours=%d, repair=%d, unify=%d\n\n",
      R.groupInHash, R.hintOnAmbiguous, R.inferEdgeHistory, R.unsplitFallback, R.familyFallback, R.topoDisc, R.repairNeedsNeighbours, R.repair, R.unify);

    // ---- baseline
    Params p0 = baseParams();
    Script s0 = makeScript(p0);
    Owners ow0 = buildOwners(s0);
    FeatRefs frefs;
    double t0 = nowMs();
    Build base = build(s0, R, true, frefs, ow0);
    double tBuild = nowMs() - t0;
    P("Baseline: %d features in script, %d built, %d failed; faces %d, edges %d, vertices %d; build %.0f ms (OCCT ops %.0f ms, propagate %.2f ms)\n\n",
      (int)s0.size(), base.builtFeatures, (int)base.failed.size(), base.idx->count(KFace), base.idx->count(KEdge), base.idx->count(KVertex),
      tBuild, base.opMs, base.propUs / 1000);
    for (auto& f : base.failed) P("- FAILED %s\n", f.c_str());
    for (auto& f : base.invalid) P("- BRepCheck invalid after %s\n", f.c_str());
    {
        GProp_GProps g; BRepGProp::VolumeProperties(base.idx->shape, g);
        P("Baseline volume %.3f mm3\n\n", g.Mass());
    }

    auto smp0 = sampleShape(*base.idx, true);
    TruthBuilder tb(ow0, *base.idx, *smp0);
    std::vector<Ref> refs;
    std::map<std::string, int> skipped, cats;
    int multiOwner = 0, withSeps = 0;
    for (int k = 0; k < 3; ++k)
        for (int i = 1; i <= base.idx->count((Kind)k); ++i) {
            SubId s{(Kind)k, i};
            TruthDesc d = k == KFace ? tb.faceDesc[i - 1] : tb.describe(s);
            if (!d.ok) { skipped[std::string(kindName((Kind)k)) + ": " + d.why]++; if (verbose && d.why != "fewer than two faces (seam)") std::printf("    skip %s #%d: %s; %s\n", kindName((Kind)k), i, d.why.c_str(), fpStr(base.idx->fingerprint(s)).c_str()); continue; }
            if (base.map.of(s).empty()) { skipped[std::string(kindName((Kind)k)) + ": element has no name"]++; continue; }
            Ref r{s, makeRef(base.map, base.nt, s), d, d.cat};
            if (k == KFace && d.cons[0].alts.size() > 1) multiOwner++;
            bool sp = !d.seps.empty(); for (auto& c : d.cons) if (!c.seps.empty()) sp = true;
            if (sp) withSeps++;
            cats[std::string(kindName((Kind)k)) + " / " + d.cat]++;
            refs.push_back(std::move(r));
        }
    int nPick = 0, nPickOk = 0;
    for (auto& [f, v] : frefs) for (auto& p : v) { nPick++; if (p.truth.ok) nPickOk++; }
    P("References recorded on the baseline: %d (plus %d feature-internal edge picks, %d of them verifiable)\n\n", (int)refs.size(), nPick, nPickOk);
    P("| kind / category | count |\n|-|-|\n");
    for (auto& [c, n] : cats) P("| %s | %d |\n", c.c_str(), n);
    P("\nElements of the baseline NOT used as references because the oracle cannot vouch for them:\n\n| reason | count |\n|-|-|\n");
    for (auto& [c, n] : skipped) P("| %s | %d |\n", c.c_str(), n);
    P("\nReferences whose face has more than one logical owner at baseline (merged faces): %d. References that needed piece separators: %d. Owner type mismatches seen: %d.\n\n",
      multiOwner, withSeps, tb.typeMismatch);

    // ---- naming statistics on the baseline
    {
        std::map<std::string, std::array<PropStats, 1>> byOp;
        std::map<std::string, std::array<int, 12>> hist; // nIn[3], vanished[3], deleted[3], count, surfMismatch
        std::map<std::string, double> us, ms; std::map<std::string, int> cnt;
        for (auto& d : base.ops) {
            byOp[d.op][0].add(d.ps);
            auto& h = hist[d.op];
            for (int k = 0; k < 3; ++k) { h[k] += d.nIn[k]; h[3 + k] += d.vanished[k]; h[6 + k] += d.deleted[k]; }
            h[9] += d.surfMismatch;
            us[d.op] += d.propUs; ms[d.op] += d.opMs; cnt[d.op]++;
        }
        P("How result elements got their names on the baseline build (faces/edges/vertices):\n\n");
        P("| op | calls | sweep origin | inherited 1:1 | split piece | merged | Generated() | inferred (A3) | neighbour fallback (no history) | names with ordinal siblings |\n|-|-|-|-|-|-|-|-|-|-|\n");
        for (auto& [op, a] : byOp) {
            const PropStats& s = a[0];
            P("| %s | %d | %d/%d/%d | %d/%d/%d | %d/%d/%d | %d/%d/%d | %d/%d/%d | %d/%d/%d | %d/%d/%d | %d/%d/%d |\n", op.c_str(), cnt[op],
              s.sweep[0], s.sweep[1], s.sweep[2], s.inherited[0], s.inherited[1], s.inherited[2], s.split[0], s.split[1], s.split[2],
              s.merged[0], s.merged[1], s.merged[2], s.generated[0], s.generated[1], s.generated[2], s.inferred[0], s.inferred[1], s.inferred[2],
              s.neighbour[0], s.neighbour[1], s.neighbour[2], s.ambiguous[0], s.ambiguous[1], s.ambiguous[2]);
        }
        P("\nOCCT history audit on the baseline build (input elements, faces/edges/vertices):\n\n");
        P("| op | inputs | gone, IsDeleted()=true | gone, IsDeleted()=false and no Modified() | Modified() result on a different surface/curve |\n|-|-|-|-|-|\n");
        for (auto& [op, h] : hist)
            if (op != "prism") P("| %s | %d/%d/%d | %d/%d/%d | %d/%d/%d | %d |\n", op.c_str(), h[0], h[1], h[2], h[6], h[7], h[8], h[3], h[4], h[5], h[9]);
        P("\nCost on the baseline build:\n\n| op | calls | OCCT op ms (mean) | propagate us (mean) |\n|-|-|-|-|\n");
        for (auto& [op, n] : cnt) P("| %s | %d | %.2f | %.0f |\n", op.c_str(), n, ms[op] / n, us[op] / n);
        double mx = 0; std::string mxf;
        std::map<std::string, double> perFeat;
        for (auto& d : base.ops) perFeat[d.feature] += d.propUs;
        for (auto& [f, u] : perFeat) if (u > mx) { mx = u; mxf = f; }
        P("\npropagate per feature: mean %.0f us, max %.0f us (%s); whole build %.2f ms of propagate against %.0f ms of OCCT operations.\n",
          base.propUs / perFeat.size(), mx, mxf.c_str(), base.propUs / 1000, base.opMs);
        P("Name table: %d names in the final ElementMap (%d elements), %.1f kB as Hash256 -> SubId; dictionary of all TopoName records ever issued in the build: %d records, %.1f kB; lineage events: %d.\n\n",
          (int)base.map.size(), base.idx->count(KFace) + base.idx->count(KEdge) + base.idx->count(KVertex), base.map.bytesHash256() / 1024.0,
          (int)base.nt.t.size(), base.nt.bytesHash256() / 1024.0, (int)base.lin.ev.size());
        // sibling groups among the referenced names
        std::map<std::string, int> amb;
        for (auto& r : refs) for (NameId n : r.ref.names) { auto it = base.nt.t.find(n); if (it != base.nt.t.end() && it->second.group > 1) amb[std::string(kindName(r.ref.kind)) + " " + it->second.role]++; }
        P("References whose name depends on an ordinal (sibling group > 1), by role:\n\n| kind role | refs |\n|-|-|\n");
        for (auto& [c, n] : amb) P("| %s | %d |\n", c.c_str(), n);
        P("\n");
    }

    // ---- scenarios
    P("| scenario | edit | built/failed | refs | Exact %% | Split %% | Merged %% | Repaired %% | Broken %% | of Broken: really gone / missed | exists: Exact+Split %% | WRONG | oracle conflicts |\n|-|-|-|-|-|-|-|-|-|-|-|-|-|\n");
    Tally total, totalPicks, first10, weakTally;
    long mutN = 0, mutCaught = 0, mutWeakN = 0, mutWeakCaught = 0, mutSplitN = 0, mutSplitCaught = 0;
    std::map<std::string, Tally> byKind, byRule;
    std::map<std::string, bool> isBlend;
    for (const Feature& f : s0) isBlend[f.id] = f.type != FType::Sweep;
    // which naming rule of PLAN 3.3 produced the (first) name of a reference
    auto ruleOf = [&](const TopoRef& ref) -> std::string {
        if (ref.names.size() > 1) return "merged element, several names";
        auto it = base.nt.t.find(ref.names[0]);
        if (it == base.nt.t.end()) return "?";
        const std::string& r = it->second.role;
        std::string k = kindName(ref.kind);
        if (r.size() > 5 && r.compare(r.size() - 5, 5, ".side") == 0) return "rule 1: sweep side face";
        if (r.find("cap") != std::string::npos) return "rule 1: sweep cap face";
        if (r == "split") return "rule 2: split piece (" + k + ")";
        if (r.find(".g") != std::string::npos) return std::string("rule 2: Generated() ") + (isBlend[it->second.origin] ? "blend face" : "section " + k);
        if (r.rfind("nb.", 0) == 0) return std::string("rule 3: neighbour-named ") + k + (isBlend[it->second.origin] ? ", after fillet/chamfer" : ", sweep tool");
        return r;
    };
    std::ostringstream wrongs, fails, missedSamples;
    int oracleConflictsTotal = 0, nWrongPrinted = 0;
    std::map<std::string, int> missedBy;
    for (const Scenario& sc : scenarios()) {
        Params p = baseParams();
        if (sc.p) sc.p(p);
        Script s = makeScript(p);
        if (sc.s) sc.s(s);
        Owners ow = buildOwners(s);
        Build b = build(s, R, false, frefs, ow);
        auto smp = sampleShape(*b.idx, true);
        Tally t;
        int conflicts = 0, existing = 0, existingOk = 0;
        auto record = [&](const std::string& what, Kind kind, const std::string& cat, const TopoRef& ref, const Resolved& r, const TruthEval& te, Verdict v, const ShapeIdx& ix) {
            if (v == VMissed) {
                std::string role = "?"; auto it = base.nt.t.find(ref.names[0]); if (it != base.nt.t.end()) role = it->second.role;
                missedBy[std::string(kindName(kind)) + " " + role + " -> " + r.how]++;
            }
            if (!isWrong(v)) return;
            std::ostringstream w;
            w << "- **" << sc.id << "** " << what << " " << kindName(kind) << " [" << cat << "]: " << verdictName(v) << ". Resolver: " << stateName(r.state) << " via " << r.how << " -> {";
            for (SubId x : r.elems) w << x.index << " ";
            w << "}; oracle expects {";
            for (int x : te.elems) w << x << " ";
            w << "}" << (te.merged ? " (merged)" : "") << ".\n  - name: " << nameStr(base.nt, ref.names[0]) << (ref.names.size() > 1 ? " (+aliases)" : "") << "\n  - picked: " << fpStr(ref.hint) << "\n";
            for (SubId x : r.elems) w << "  - got: " << fpStr(ix.fingerprint(x)) << "\n";
            for (int x : te.elems) { w << "  - expected: " << fpStr(ix.fingerprint({kind, x})); const auto& nm = b.map.of({kind, x}); if (!nm.empty()) w << " named " << nameStr(b.nt, nm[0]); w << "\n"; }
            if (nWrongPrinted++ < 60) wrongs << w.str();
        };
        for (const Ref& r : refs) {
            Resolved res = resolve(r.ref, b.map, b.nt, b.lin, R);
            TruthEval te = evalTruth(r.truth, ow, *b.idx, *smp);
            std::vector<int> cong;
            for (int i = 1; i <= b.idx->count(r.ref.kind); ++i) if (fpClose(r.ref.hint, b.idx->fingerprint({r.ref.kind, i}))) cong.push_back(i);
            Verdict v = r.truth.weak ? judgeWeak(res, te, cong.size() == 1 ? cong[0] : 0) : judge(res, te);
            t.add(res.state, v);
            if (r.truth.weak) weakTally.add(res.state, v);
            // Mutation test of the oracle: swap a correct single answer for its nearest neighbour of the same
            // kind and geometry type; drop one element of a correct Split. The oracle must call both wrong.
            if (v == VCorrect && res.elems.size() == 1) {
                const Fingerprint& fx = b.idx->fingerprint(res.elems[0]);
                int best = 0; double bd = 1e300;
                for (int i = 1; i <= b.idx->count(r.ref.kind); ++i) {
                    if (i == res.elems[0].index) continue;
                    const Fingerprint& fi = b.idx->fingerprint({r.ref.kind, i});
                    if (fi.type != fx.type) continue;
                    double d = std::hypot(double(fi.qCx - fx.qCx), double(fi.qCy - fx.qCy), double(fi.qCz - fx.qCz));
                    if (d < bd) { bd = d; best = i; }
                }
                if (best) {
                    Resolved mres = res; mres.elems = {{r.ref.kind, best}};
                    Verdict mv = r.truth.weak ? judgeWeak(mres, te, cong.size() == 1 ? cong[0] : 0) : judge(mres, te);
                    if (r.truth.weak) { mutWeakN++; if (isWrong(mv)) mutWeakCaught++; }
                    else { mutN++; if (isWrong(mv)) mutCaught++; else if (verbose) std::printf("    MUTANT NOT CAUGHT %s %s base#%d\n", sc.id.c_str(), kindName(r.ref.kind), r.base.index); }
                }
            } else if (v == VCorrect && res.elems.size() > 1 && !r.truth.weak) {
                Resolved mres = res; mres.elems.pop_back();
                mutSplitN++; if (isWrong(judge(mres, te))) mutSplitCaught++;
            }
            byKind[kindName(r.ref.kind)].add(res.state, v);
            byRule[ruleOf(r.ref)].add(res.state, v);
            if (!te.elems.empty()) { existing++; if (res.state == RState::Exact || res.state == RState::Split || res.state == RState::Merged) existingOk++; }
            // second, weaker, independent check: an element that is congruent in place must be the oracle's answer
            if (!r.truth.weak && cong.size() == 1 && !(te.elems.size() == 1 && te.elems[0] == cong[0])) {
                conflicts++;
                if (verbose && conflicts <= 3) std::printf("    oracle conflict %s %s base#%d cong=%d oracle n=%d %s\n", sc.id.c_str(), kindName(r.ref.kind), r.base.index, cong[0], (int)te.elems.size(), nameStr(base.nt, r.ref.names[0]).c_str());
            }
            record("ref", r.ref.kind, r.cat, r.ref, res, te, v, *b.idx);
        }
        Tally tp;
        for (const PickOutcome& po : b.picks) { tp.add(po.r.state, po.v); if (isWrong(po.v)) { std::ostringstream w; w << "- **" << sc.id << "** feature pick " << po.feature << " target " << po.target << ": " << verdictName(po.v) << " (" << stateName(po.r.state) << " via " << po.r.how << ", got " << po.r.elems.size() << ", expected " << po.te.elems.size() << ")\n"; wrongs << w.str(); } }
        total.merge(t); totalPicks.merge(tp);
        {
            // PLAN 3.3 acceptance set: one dimension change on each of the first ten features, added sketch
            // lines on four of their sketches, and reorders of independent features among them.
            static const std::set<std::string> plan33 = {"D01", "D01h", "D02", "D35", "D03", "D04", "D05", "D06", "D07", "D08", "D09",
                                                         "K31", "K32", "K33", "K34", "O19", "O20", "O22"};
            if (plan33.count(sc.id)) first10.merge(t);
        }
        oracleConflictsTotal += conflicts;
        P("| %s | %s | %d/%d | %d | %s | %s | %s | %s | %s | %d / %d | %s | %d | %d |\n", sc.id.c_str(), sc.desc.c_str(), b.builtFeatures, (int)b.failed.size(), t.n,
          pct(t.st[0], t.n).c_str(), pct(t.st[1], t.n).c_str(), pct(t.st[2], t.n).c_str(), pct(t.st[3], t.n).c_str(), pct(t.st[4], t.n).c_str(),
          t.v[VBrokenGone], t.v[VMissed], pct(existingOk, existing).c_str(), t.wrong() + tp.wrong(), conflicts);
        for (auto& f : b.failed) fails << "- " << sc.id << ": " << f << "\n";
        for (auto& f : b.invalid) fails << "- " << sc.id << ": BRepCheck invalid after " << f << "\n";
        if (sc.id == "N00" && (t.st[0] != t.n)) P("\n**DETERMINISM FAILURE: no-op rebuild did not resolve everything Exact**\n\n");
    }
    auto row = [&](const char* name, const Tally& t) {
        P("| %s | %d | %s | %s | %s | %s | %s | %d | %d | %d | %d | %d | %d | **%d** |\n", name, t.n, pct(t.st[0], t.n).c_str(), pct(t.st[1], t.n).c_str(), pct(t.st[2], t.n).c_str(),
          pct(t.st[3], t.n).c_str(), pct(t.st[4], t.n).c_str(), t.v[VBrokenGone], t.v[VMissed], t.v[VWrongOther], t.v[VWrongIncomplete], t.v[VWrongGhost], t.v[VSilentMerge], t.wrong());
    };
    P("\n| set | resolutions | Exact %% | Split %% | Merged %% | Repaired %% | Broken %% | broken, really gone | broken, missed | wrong: different element | wrong: incomplete set | wrong: ghost | wrong: unflagged merge | WRONG total |\n|-|-|-|-|-|-|-|-|-|-|-|-|-|-|\n");
    row("all scenarios, recorded refs", total);
    for (auto& [k, t] : byKind) row((std::string("  ") + k + " refs").c_str(), t);
    row("PLAN 3.3 set (D01-D09, D01h, D35, K31-K34, O19, O20, O22)", first10);
    for (auto& [k, t] : byRule) row(("  " + k).c_str(), t);
    row("  of which weak-oracle refs", weakTally);
    row("feature-internal picks (fillet/chamfer edges)", totalPicks);
    P("\nOracle conflicts (element congruent in place but oracle disagrees): %d\n\n", oracleConflictsTotal);
    P("Mutation test of the oracle: %ld correct single answers replaced by the nearest other element of the same kind and geometry type, %ld flagged wrong (%.2f%%); "
      "%ld correct Split answers with one piece removed, %ld flagged wrong; weak-oracle refs: %ld mutants, %ld flagged.\n\n",
      mutN, mutCaught, mutN ? 100.0 * mutCaught / mutN : 0.0, mutSplitN, mutSplitCaught, mutWeakN, mutWeakCaught);
    P("Missed references (Broken although the element still exists), by baseline name role and resolver reason:\n\n| kind role -> reason | count |\n|-|-|\n");
    for (auto& [c, n] : missedBy) P("| %s | %d |\n", c.c_str(), n);
    P("\nFeature failures during scenario rebuilds:\n\n%s\n", fails.str().empty() ? "(none)\n" : fails.str().c_str());
    md << "\nWrong-element resolutions (first 60):\n\n" << (wrongs.str().empty() ? "(none)\n" : wrongs.str());
    std::fputs(("\nWrong-element resolutions:\n" + (wrongs.str().empty() ? std::string("(none)\n") : wrongs.str())).c_str(), stdout);
    RunOut out; out.md = md.str(); out.wrong = total.wrong() + totalPicks.wrong();
    return out;
}

int main(int argc, char** argv) {
    bool verbose = argc > 1 && std::strcmp(argv[1], "-v") == 0;
    std::printf("OCCT %s\n", OCC_VERSION_COMPLETE);
    if (argc > 1 && std::strcmp(argv[1], "-h") == 0) { runHistoryProbes(); return 0; }
    if (argc > 1 && std::strcmp(argv[1], "-x") == 0) { // experiment: which bottom chamfer picks does OCCT accept?
        Params p = baseParams(); Script sc = makeScript(p); Owners ow = buildOwners(sc);
        Script pre; Feature f35; for (auto& f : sc) { if (f.id == "F35") { f35 = f; break; } pre.push_back(f); }
        if (argc > 2) for (const char* c = argv[2]; *c; ++c) { if (*c == 'n') { del(pre, "F21"); del(pre, "F22"); } if (*c == 'f') del(pre, "F02"); if (*c == 't') { del(pre, "F32"); del(pre, "F33"); del(pre, "F34"); } if (*c == 'h') { del(pre, "F10"); del(pre, "F31"); del(pre, "F09"); del(pre, "F12"); del(pre, "F25"); } }
        Rules R; FeatRefs fr; Build b = build(pre, R, true, fr, ow);
        for (auto& x : b.invalid) std::printf("pre invalid after %s\n", x.c_str());
        auto smp = sampleShape(*b.idx, false);
        for (double d : {1.0, 0.8, 0.5}) for (int k = 0; k < (int)f35.targets.size(); ++k) {
            auto pk = pickTarget(f35.targets[k], ow, *b.idx, *smp);
            for (auto& [e, desc] : pk) { OpOut o = opBlend(false, *b.idx, {e}, d); std::printf("d=%.1f target %d (%s,%s) edge %d: %s %s\n", d, k, f35.targets[k].a.c_str(), f35.targets[k].b.c_str(), e, o.ok ? (o.valid ? "ok" : "ok-but-INVALID") : "FAIL", o.err.c_str()); }
        }
        return 0;
    }
    std::vector<Rules> sets;
    { Rules r; r.label = "v0 (PLAN 3.3 as written)"; sets.push_back(r); }
    { Rules r; r.label = "v1 (safety amendments A1 A2 A8)"; r.groupInHash = true; r.hintOnAmbiguous = true; r.repairNeedsNeighbours = true; sets.push_back(r); }
    { Rules r; r.label = "v2 (v1 + A3 A4 A5 A7)"; r.groupInHash = true; r.hintOnAmbiguous = true; r.repairNeedsNeighbours = true;
      r.inferEdgeHistory = true; r.unsplitFallback = true; r.familyFallback = true; r.topoDisc = true; sets.push_back(r); }
    if (argc > 2) { int only = std::atoi(argv[2]); sets = {sets[only]}; }
    std::ofstream f("results.md");
    f << "# S4 raw results (generated by s4.exe, OCCT " << OCC_VERSION_COMPLETE << ")\n";
    for (const Rules& r : sets) {
        RunOut o = runAll(r, verbose);
        f << o.md;
        std::printf("\n==> %s: WRONG = %d\n", r.label.c_str(), o.wrong);
    }
    return 0;
}
