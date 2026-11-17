// Spike S4: persistent naming prototype. Throwaway code. See REPORT.md.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>
#include <gp_Vec.hxx>

namespace s4 {

enum Kind : std::uint8_t { KFace = 0, KEdge = 1, KVertex = 2 };
inline const char* kindName(Kind k) { return k == KFace ? "face" : k == KEdge ? "edge" : "vertex"; }
struct SubId {
    Kind kind{KFace};
    int index{0}; // 1-based index in the canonical TopExp::MapShapes order of one shape; 0 = invalid
    auto operator<=>(const SubId&) const = default;
};
using NameId = std::uint64_t; // prototype: 64-bit FNV-1a with collision check. Product: Hash256.

// ------------------------------------------------------------------ script (the parametric part)
struct Ent {
    char k{'L'};            // 'L' line, 'A' ccw arc, 'C' full circle
    double x0{}, y0{}, x1{}, y1{}; // endpoints (line, arc)
    double cx{}, cy{}, r{};        // arc / circle
    int tag{};              // stable sketch entity id (unique inside the feature)
};
struct Loop { int id{}; std::vector<Ent> ents; }; // closed, counter-clockwise in sketch coordinates
struct Frame {
    gp_Pnt o; gp_Dir xd{1, 0, 0}; gp_Dir n{0, 0, 1};
    gp_Dir yd() const { return n.Crossed(xd); }
    gp_Pnt at(double u, double v, double w = 0) const {
        return gp_Pnt(o.XYZ() + u * xd.XYZ() + v * yd().XYZ() + w * n.XYZ());
    }
};
enum class FType { Sweep, Fillet, Chamfer };
enum class BOp { New, Join, Cut };
// A logical edge to blend: intersection of two logical faces, named by owner key (see truth).
struct Target { std::string a, b; bool convex{true}; bool picked{true}; };
struct Feature {
    std::string id;
    FType type{FType::Sweep};
    BOp op{BOp::New};
    std::string role;       // "extrude", "hole", "fillet", "chamfer"
    Frame frame;
    std::vector<Loop> loops;
    double dist{};          // sweep length along dir (dir = +n for New/Join, -n for Cut)
    std::vector<Target> targets;
    double size{};          // fillet radius / chamfer distance
    bool suppressed{false};
    gp_Dir dir() const { return op == BOp::Cut ? n().Reversed() : n(); }
    gp_Dir n() const { return frame.n; }
};
using Script = std::vector<Feature>;
using Params = std::map<std::string, double>;
Params baseParams();
Script makeScript(const Params&);

// ------------------------------------------------------------------ kernel layer (OCCT)
struct Fingerprint {
    int type{};                                  // GeomAbs surface / curve type, -1 for vertex
    std::int64_t qArea{}, qCx{}, qCy{}, qCz{}, qRadius{};
    std::int64_t qAxisX{}, qAxisY{}, qAxisZ{};
    std::uint16_t neighbourCount{};
    auto operator<=>(const Fingerprint&) const = default;
    // sort key of PLAN 3.3 rule 4: centroid, then area, then axis
    auto key() const { return std::tie(qCx, qCy, qCz, qArea, qAxisX, qAxisY, qAxisZ, qRadius, type); }
};
struct ShapeIdx {
    TopoDS_Shape shape;
    TopTools_IndexedMapOfShape m[3];
    TopTools_IndexedDataMapOfShapeListOfShape EF, VF, VE;
    mutable std::vector<std::optional<Fingerprint>> fp[3];
    int count(Kind k) const { return m[k].Extent(); }
    SubId find(const TopoDS_Shape& s) const;
    const TopoDS_Shape& get(SubId s) const { return m[s.kind](s.index); }
    std::vector<int> facesOfEdge(int e) const;
    std::vector<int> facesOfVertex(int v) const;
    std::vector<int> edgesOfFace(int f) const;
    std::vector<int> edgesOfVertex(int v) const;
    std::vector<int> verticesOfEdge(int e) const;
    std::vector<int> around(Kind k, int i) const; // faces adjacent to a face / incident to an edge or vertex
    const Fingerprint& fingerprint(SubId s) const;
};
std::shared_ptr<ShapeIdx> indexShape(const TopoDS_Shape&);

struct HistoryEntry {
    enum Rel : std::uint8_t { Modified, Generated, Deleted } rel;
    int input;
    SubId src;
    std::vector<SubId> res;
    bool kept{false}; // Modified synthesised because the same TShape is still in the result
};
struct ShapeHistory {
    std::vector<HistoryEntry> entries;
    // diagnostics (per kind)
    int vanished[3]{};   // input element: not in result, no Modified, IsDeleted false
    int deletedOk[3]{};
    int nIn[3]{};
};
struct SweepOrigin { SubId face; NameId parent; std::string role; };
struct OpOut {
    bool ok{false};
    std::string err;
    std::shared_ptr<ShapeIdx> result;
    ShapeHistory hist;
    std::vector<SweepOrigin> origins;
    double ms{};
    bool valid{true};
};
NameId hashStr(const std::string&);
OpOut opSweep(const Feature&);
OpOut opBoolean(BOp, const ShapeIdx& target, const ShapeIdx& tool, bool unify);
OpOut opBlend(bool fillet, const ShapeIdx& target, const std::vector<int>& edges, double size);

// ------------------------------------------------------------------ naming (the thing under test)
struct TopoName {
    NameId id{};
    Kind kind{};
    std::string origin;
    std::string role;
    std::vector<NameId> parents;
    std::uint32_t ordinal{};
    std::uint32_t group{1};   // number of siblings sharing (origin, role, parents, disc) when it was made
    std::vector<NameId> disc; // A7: topological discriminator among siblings (names of faces around the piece)
    NameId family{};          // hash of (kind, origin, role, parents): all pieces of one logical element
};
struct NameTable {
    std::unordered_map<NameId, TopoName> t;
    std::unordered_map<NameId, std::vector<NameId>> families; // family -> names issued in this build
    NameId intern(TopoName& n, bool groupInHash);
    std::size_t bytesHash256() const;
};
struct ElementMap {
    std::shared_ptr<ShapeIdx> idx;
    std::vector<std::vector<NameId>> names[3]; // [kind][index-1]; sorted; >1 entries = merged element
    std::unordered_map<NameId, SubId> byName;
    const std::vector<NameId>& of(SubId s) const { return names[s.kind][s.index - 1]; }
    std::size_t size() const { return byName.size(); }
    std::size_t bytesHash256() const { return byName.size() * (32 + 5); }
};
struct LineageEvent {
    enum K : std::uint8_t { Split, Merged, Deleted } kind;
    NameId from;
    std::vector<NameId> to;
    std::string cause;
};
struct Lineage {
    std::vector<LineageEvent> ev;
    std::unordered_multimap<NameId, int> byFrom;
    void add(LineageEvent e) { byFrom.emplace(e.from, (int)ev.size()); ev.push_back(std::move(e)); }
};
struct Rules {
    std::string label = "v0";
    bool groupInHash = false;     // A1: sibling-group size is part of the hashed name
    bool hintOnAmbiguous = false; // A2: a name with group>1 is only trusted if the stored hint still matches
    bool inferEdgeHistory = false;// A3: infer Modified for history-less edges/vertices from face history
    bool unsplitFallback = false; // A4: a "split" name whose parent is whole again resolves as Merged
    bool repair = true;           // fingerprint repair (naming.h Repaired)
    bool repairNeedsNeighbours = false; // A8: repair also requires the picked element's neighbour names to agree
    bool familyFallback = false;  // A5: a lost name resolves through its family (Split / Merged), never Exact
    bool topoDisc = false;        // A7: siblings are told apart by neighbouring face names before any ordinal
    bool unify = true;            // booleans call SimplifyResult()
};
struct NamingInput {
    std::string feature, role;
    std::vector<const ElementMap*> inputs;
    const ShapeHistory* hist{nullptr};
    std::shared_ptr<ShapeIdx> result;
    const std::vector<SweepOrigin>* origins{nullptr};
};
struct PropStats {
    int sweep[3]{}, inherited[3]{}, split[3]{}, merged[3]{}, generated[3]{}, neighbour[3]{}, inferred[3]{};
    int ambiguous[3]{}; // names issued with group > 1
    int unnamed[3]{};
    void add(const PropStats& o);
};
ElementMap propagate(const NamingInput&, NameTable&, Lineage&, const Rules&, PropStats&);

struct TopoRef {
    Kind kind{};
    std::vector<NameId> names; // DEVIATION from naming.h: all names of the picked element, not one
    Fingerprint hint;
    std::vector<TopoName> recs;   // DEVIATION: the full TopoName records of `names` (parents, family, group)
    std::vector<NameId> nbNames;  // DEVIATION: primary names of the faces around the element at pick time
};
enum class RState { Exact, Split, Merged, Repaired, Broken };
inline const char* stateName(RState s) {
    switch (s) { case RState::Exact: return "Exact"; case RState::Split: return "Split";
        case RState::Merged: return "Merged"; case RState::Repaired: return "Repaired"; default: return "Broken"; }
}
struct Resolved { RState state{RState::Broken}; std::vector<SubId> elems; std::string how; };
TopoRef makeRef(const ElementMap&, const NameTable&, SubId);
Resolved resolve(const TopoRef&, const ElementMap&, const NameTable&, const Lineage&, const Rules&);
bool fpClose(const Fingerprint& a, const Fingerprint& b);

// ------------------------------------------------------------------ ground truth (independent of naming)
struct Carrier {
    enum K { None, Plane, Cyl, Torus, Cone } k{None};
    gp_Pnt p; gp_Dir a{0, 0, 1};
    double r{}, r2{};          // Cyl: r. Torus: major r, minor r2.
    double q[4]{};             // Cone: (rho0, z0, rho1, z1) of two points of the generator, z along a from p
    int sigma{+1};             // Cyl: +1 material on the axis side (boss), -1 hole. Plane: unused
    bool oriented{false};      // true: `a` is the outward normal (Plane) / sigma is known (Cyl) and is checked
    double dist(const gp_Pnt&) const; // signed where meaningful (plane: along outward normal; cyl: radial - r)
};
struct Owner {
    std::string key;
    Carrier c;
    std::function<bool(const gp_Pnt&, double)> in; // region predicate with margin
    std::string cat;
};
struct Owners {
    std::vector<Owner> v;
    std::map<std::string, int> ix;
    const Owner* get(const std::string& k) const { auto it = ix.find(k); return it == ix.end() ? nullptr : &v[it->second]; }
};
Owners buildOwners(const Script&);

struct ElemSamples {
    std::vector<gp_Pnt> pts; std::vector<double> w;   // interior samples with weights
    std::vector<gp_Pnt> nodes;                        // points that must lie on the carrier
    int type{-1};
    gp_Dir nrm{0, 0, 1};  // planar face: outward normal
    int sigma{0};         // cylindrical face: +1 outward normal points away from the axis (boss), -1 hole
};
struct Samples { std::vector<ElemSamples> e[3]; };
std::shared_ptr<Samples> sampleShape(const ShapeIdx&, bool withFaces);

struct Sep { std::string owner; int side; Carrier frozen; }; // frozen: baseline geometry, used if the owner is gone
struct Constraint { std::vector<std::string> alts; std::vector<Sep> seps; };
struct TruthDesc {
    Kind kind{};
    std::vector<Constraint> cons;
    std::vector<Sep> seps;
    bool ok{false};
    bool weak{false}; // pieces that no analytic carrier separates (seam-induced): only weaker checks are possible
    std::string why;
    std::string cat;
};
struct TruthEval { std::vector<int> elems; bool merged{false}; bool droppedSep{false}; };
TruthEval evalTruth(const TruthDesc&, const Owners&, const ShapeIdx&, const Samples&);
// Build the descriptor of baseline element `s` (faces first, then edges/vertices use the face descriptors).
struct TruthBuilder {
    const Owners& ow; const ShapeIdx& idx; const Samples& smp;
    std::vector<TruthDesc> faceDesc; // per face
    int typeMismatch{0};
    TruthBuilder(const Owners& o, const ShapeIdx& i, const Samples& s);
    TruthDesc describe(SubId s);
    bool addSeparators(TruthDesc& d, int self, std::vector<Sep>& where);
};
// Descriptor of a logical edge A∩B picked on `idx` (used for fillet/chamfer picks).
std::vector<std::pair<int, TruthDesc>> pickTarget(const Target&, const Owners&, const ShapeIdx&, const Samples&);

void runHistoryProbes();

} // namespace s4
