// The naming oracle (task T-081a): decides, from geometry alone, which element of a built shape a reference
// should resolve to. Promoted from spike S4 (spikes/s4-naming/truth.cpp) and extended to imported parts.
//
// INDEPENDENCE. This library links ray_kernel and ray_core only, never ray_naming, and includes no naming header:
// it never sees a name, a history entry, a fingerprint of the naming code, or an index from another build. It
// knows (a) the script (script_model.h): the parameters of every sweep and blend, and the imported base shape,
// and (b) the geometry of a built shape through the public kernel queries (tessellate, surfaceInfo, curveInfo,
// vertexPoint, adjacent). No kernel history is used.
//
// IDENTITY. Every logical face of the script is an "owner": a carrier surface (plane, cylinder, cone, torus or
// sphere, from the script in closed form; for an imported face, its own surface, or its fine mesh for B-splines)
// plus an extent (the sketch region swept, the neighbourhood of the blended edge, or the imported face itself).
// A face of a built shape is described by the owner(s) whose carrier all its mesh nodes lie on and whose extent
// covers it; when one logical face is in several pieces, also by the side of other owners' carriers each piece
// lies on ("separators"). An edge is described by the descriptions of its faces (it lies on both carriers), a
// vertex by those of all its faces, each with separators if needed. After an edit the owners are recomputed from
// the edited script and the elements of the new shape that fit the description are the expected answer E
// (possibly empty, several, or "merged" when a matching face extends beyond the description).
//
// WHAT IT CAN MISS (also in the T-081a report):
//  - It shares the script's idea of identity: "the same element" means the same sketch entity, cap, blended edge
//    or imported face. An error both the naming code and the script make (for example two sketch lines swapping
//    places) is invisible.
//  - Edges and vertices have no orientation test: two different edges on the same two carriers inside both extents
//    are pieces of one logical edge unless a separator tells them apart.
//  - Elements whose description needs no separator but whose carriers coincide with another owner's (two logical
//    faces on one plane with the same material side and overlapping extents) cannot be told apart; such faces are
//    described by both owners ("merged") and judged as one.
//  - Weak references (seam-induced pieces, vertices on fewer than three faces) are checked only for membership of
//    the logical element, not for which piece.
//  - Sampling: a face counts as part of a region from 2 % of its area and as fully inside from 98 %; slivers below
//    2 % are ignored. Edges count from 5 %, vertices from 50 %.
//  - Only the carriers listed above. A blend between faces that are not plane/plane or plane/coaxial cylinder has
//    no owner, so its faces cannot be described and are never used as references; scripts avoid them.
#pragma once
#include <ray/kernel.h>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ray::naming_oracle {

using kernel::SubId;
using kernel::SubKind;

// ---------------------------------------------------------------- vectors
inline Vec3 add(const Vec3& a, const Vec3& b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec3 sub(const Vec3& a, const Vec3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec3 mul(const Vec3& a, double s)
{
    return {a.x * s, a.y * s, a.z * s};
}
inline double dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double norm(const Vec3& a);
Vec3 unit(const Vec3& a);

// ---------------------------------------------------------------- the script (the test input)
struct Ent {
    char k = 'L';                          // 'L' line, 'A' counter-clockwise arc, 'C' full circle
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0; // end points (line, arc)
    double cx = 0, cy = 0, r = 0;          // arc, circle
    std::uint64_t tag = 0;                 // stable sketch entity id, unique inside the feature
};
struct Loop {
    std::uint64_t id = 0;  // stable region id
    std::vector<Ent> ents; // closed, counter-clockwise about the frame normal
};
struct Frame {
    Vec3 o{0, 0, 0};
    Vec3 xd{1, 0, 0};
    Vec3 n{0, 0, 1};
    Vec3 yd() const { return cross(n, xd); }
    Vec3 at(double u, double v, double w = 0) const { return add(add(add(o, mul(xd, u)), mul(yd(), v)), mul(n, w)); }
};
enum class FType { Import, Sweep, Fillet, Chamfer };
enum class BOp { New, Join, Cut };
// A logical edge to blend: where owner `a` meets owner `b`. `convex` says on which side the material is.
// `picked` targets are picked by the user; the others are blended by tangent propagation and listed so the
// oracle knows their faces.
struct Target {
    std::string a, b;
    bool convex = true;
    bool picked = true;
};

// What the oracle knows about one face of an imported shape.
struct FaceMesh {
    std::vector<Vec3> nodes;
    std::vector<std::array<std::uint32_t, 3>> triangles;
    std::vector<std::array<Vec3, 2>> triangleBoxes; // min, max
    Vec3 boxMin{}, boxMax{};
    // Distance from p to the triangles, if it is at most `limit`; else a value above `limit`.
    double distanceWithin(const Vec3& p, double limit) const;
};
struct Carrier {
    enum class K { None, Plane, Cyl, Torus, Cone, Sphere, Mesh } k = K::None;
    Vec3 p{};
    Vec3 a{0, 0, 1};
    double r = 0, r2 = 0;  // Cyl, Sphere: r. Torus: major r, minor r2.
    double q[4]{};         // Cone: (rho0, z0, rho1, z1), two points of the generator, z along a from p
    int sigma = +1;        // Cyl: +1 material on the axis side (boss), -1 hole
    bool oriented = false; // Plane: `a` is the outward normal; Cyl: sigma is known; both are checked
    kernel::SurfaceType surface = kernel::SurfaceType::Other; // Mesh: the surface type faces must have
    std::shared_ptr<const FaceMesh> mesh;                     // Mesh
    double meshTolerance = 0;                                 // Mesh: how far the mesh may be from the surface
    // Signed where meaningful (plane: along the outward normal; cylinder: radial - r). Not for Mesh.
    double dist(const Vec3& x) const;
    // |dist| <= tol; for Mesh, within tol of the mesh (plus its tolerance).
    bool on(const Vec3& x, double tol) const;
    bool analytic() const { return k != K::None && k != K::Mesh; }
};
struct ImportBase {
    kernel::Shape shape;
    std::vector<Carrier> carriers;                       // per face of `shape`
    std::vector<std::shared_ptr<const FaceMesh>> meshes; // per face
    double meshTolerance = 0;
};
// Analyses an imported shape once: carrier and fine mesh of every face. KernelFailure if the kernel fails.
Result<std::shared_ptr<const ImportBase>> analyseImport(const kernel::Shape& shape);

struct Feature {
    std::string id;
    FType type = FType::Sweep;
    BOp op = BOp::New;
    std::string role; // "import", "extrude", "hole", "fillet", "chamfer"
    Frame frame;
    std::vector<Loop> loops; // each loop is its own region; Cut extrudes along -n, New and Join along +n
    double dist = 0;
    std::vector<Target> targets;
    double size = 0; // fillet radius, chamfer distance
    bool suppressed = false;
    std::shared_ptr<const ImportBase> base; // Import
};
using Script = std::vector<Feature>;

// ---------------------------------------------------------------- owners
struct Owner {
    std::string key; // "<feature>/s<tag>" side, "<feature>/c<0|1>/<loop>" cap, "<feature>/b<k>" blend,
                     // "<feature>/i<face>" imported face
    Carrier c;
    std::function<bool(const Vec3&, double)> in; // inside the extent, with a margin
    std::string cat;
};
struct Owners {
    std::vector<Owner> v;
    std::map<std::string, int> ix;
    const Owner* get(const std::string& key) const
    {
        const auto it = ix.find(key);
        return it == ix.end() ? nullptr : &v[static_cast<std::size_t>(it->second)];
    }
};
Owners buildOwners(const Script& script);
// True if the oracle can describe a blend of `target` (both owners analytic: plane/plane not parallel, or a plane
// and a cylinder on an axis square to it).
bool canBlend(const Owners& owners, const Target& target, bool fillet);

// ---------------------------------------------------------------- samples of a built shape
struct ElemSamples {
    std::vector<Vec3> pts;   // interior samples
    std::vector<double> w;   // their weights
    std::vector<Vec3> nodes; // points that lie exactly on the element
    int type = -1;           // kernel::SurfaceType for faces, kernel::CurveType for edges, -1 for vertices
    Vec3 nrm{0, 0, 1};       // planar face: outward normal
    int sigma = 0;           // cylindrical face: +1 boss, -1 hole
    Vec3 boxMin{}, boxMax{};
    std::shared_ptr<const FaceMesh> mesh; // face: its triangles
};
struct Samples {
    std::array<std::vector<ElemSamples>, 3> e;                      // by SubKind (Vertex, Edge, Face)
    std::array<std::vector<std::vector<std::uint32_t>>, 2> facesOf; // [0] faces of each vertex, [1] of each edge
    const std::vector<ElemSamples>& of(SubKind k) const { return e[static_cast<std::size_t>(k)]; }
};
Result<std::shared_ptr<const Samples>> sampleShape(const kernel::Shape& shape);

// ---------------------------------------------------------------- descriptions and verdicts
// A separator: the piece lies on side `side` of the owner's carrier, wherever a point's projection onto the
// carrier falls inside the owner's extent ("in band"). Elsewhere the separator says nothing: a carrier is an
// infinite surface, and when its feature moves, the far part of the plane must not decide which piece a distant
// point belongs to.
struct Sep {
    std::string owner;
    int side = 0;
    Carrier frozen;                                    // baseline geometry, used when the owner no longer exists
    std::function<bool(const Vec3&, double)> frozenIn; // and its baseline extent
};
struct Constraint {
    std::vector<std::string> alts; // the element lies on one of these owners
    std::vector<Sep> seps;
};
struct TruthDesc {
    SubKind kind = SubKind::Face;
    std::vector<Constraint> cons;
    std::vector<Sep> seps;
    bool ok = false;
    bool weak = false; // pieces no analytic carrier separates: only membership is checked
    std::string why;
    std::string cat;
};
struct TruthEval {
    std::vector<std::uint32_t> elems; // sorted
    bool merged = false;              // a matching element extends beyond the description
};
TruthEval evalTruth(const TruthDesc& d, const Owners& owners, const Samples& samples);
// For diagnosis: the description in words, and why element `x` does or does not fit it.
std::string explain(const TruthDesc& d, const Owners& owners, const Samples& samples, long x);

class TruthBuilder {
public:
    TruthBuilder(const Owners& owners, const Samples& samples);
    // The description of element s of the shape the samples came from; d.ok false (with d.why) if the oracle
    // cannot vouch for it (it must select exactly that element on this shape).
    TruthDesc describe(SubId s) const;
    const std::vector<TruthDesc>& faceDescriptions() const { return faceDesc_; }

private:
    const Owners& owners_;
    const Samples& samples_;
    std::vector<TruthDesc> faceDesc_;
};

// The edges of the shape on the logical edge `target`, each with its own description.
std::vector<std::pair<std::uint32_t, TruthDesc>> pickTarget(const Target& target, const Owners& owners,
                                                            const Samples& samples);

enum class Verdict { Correct, BrokenGone, Missed, WrongOther, WrongIncomplete, WrongGhost, WrongSilentMerge };
const char* verdictName(Verdict v);
inline bool isWrong(Verdict v)
{
    return v >= Verdict::WrongOther;
}
// The answer of a resolver, in the oracle's terms.
enum class Answer { Exact, Split, Merged, Repaired, Broken };
// `answer` with `elements` against the expected answer `te`. `congruent` (weak descriptions only) is the one
// element congruent in place with the picked one, or -1.
Verdict judge(Answer answer, std::vector<std::uint32_t> elements, const TruthEval& te, bool weak, long congruent);

// NEW TERRITORY. A second look at a wrong verdict, for one situation the description cannot settle: a part of the
// picked element's logical face (or edge) that did not exist when it was picked shows up after the edit (a piece
// that a later cut had removed survives, or an edge that a cut had trimmed now runs on past it). Whether such a
// part belongs to the reference is a matter of definition: a description with separators says no, one without
// says yes. The same happens when a separator's band (see Sep) no longer reaches the continuation of a sibling
// piece after an edit. The answer is accepted when (1) it includes every expected element that overlaps the picked
// element (all expected elements if none does: the element moved), (2) every answered element that was not
// expected lies on the picked element's own owners (the description without separators) and overlaps neither the
// picked element nor any other element of the baseline, and (3) for an Exact answer on an element larger than the
// description, the part outside it overlaps no other element of the baseline. Expected elements that do not
// overlap the picked element may be left out. Anything else stays wrong: an answer missing what was picked, an
// answer with an element of another logical face, with a part of another baseline element, or a ghost. Returns the
// verdict to count; `newTerritory` is set when it changed the verdict. "Other element of the baseline" counts only one
// that `survives` the edit (its own description still finds it; true when it has none) and, for faces, lies on
// the same plane.
Verdict reconsider(Verdict v, Answer answer, const std::vector<std::uint32_t>& elements, const TruthDesc& d,
                   const TruthEval& expected, const Owners& owners, const Samples& now, const Samples& baseline,
                   SubId picked, const std::function<bool(SubId)>& survives, bool& newTerritory);

// Elements of `samples` whose nodes all lie within `tol` of the picked element's nodes and vice versa
// (congruent in place). Independent of the description; a cross-check of it.
std::vector<std::uint32_t> congruent(const ElemSamples& picked, SubKind kind, const Samples& samples, double tol);

} // namespace ray::naming_oracle
