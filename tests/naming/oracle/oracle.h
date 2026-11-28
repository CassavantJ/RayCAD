// The naming oracle (task T-081a): decides, from geometry alone, which element of a built shape a reference
// should resolve to. Promoted from spike S4 (spikes/s4-naming/truth.cpp) and extended to imported parts.
//
// INDEPENDENCE. This library links ray_kernel and ray_core only, never ray_naming, and includes no naming header:
// it never sees a name, a history entry, a fingerprint of the naming code, or an index from another build. It
// knows (a) the script (Feature, below): the parameters of every sweep and blend, and the imported base shape,
// and (b) the geometry of a built shape through the public kernel queries (tessellate, surfaceInfo, curveInfo,
// vertexPoint, adjacent). No kernel history is used.
//
// IDENTITY. Every logical face of the script is an "owner": a carrier surface (plane, cylinder, cone, torus or
// sphere, from the script in closed form; for an imported face, its own surface, or its fine mesh for B-splines)
// plus an extent (the sketch region swept, the neighbourhood of the blended edge, or the imported face itself).
// A face of a built shape is described by the owner(s) whose carrier its mesh nodes lie on (with its surface type
// and material side) and whose extent covers it; when one logical face is in several pieces, also by the side of
// other owners' carriers each piece lies on ("separators", applied only where a point projects into the
// separator owner's extent). An edge is described by the owners of its faces (it lies on both carriers, and
// borders a face lying on each with that face's type and material side), a vertex by those of all its faces, each
// with separators of its own if needed. After an edit the owners are recomputed from the features of the edited
// script that built, and the elements of the new shape that fit the description are the expected answer E
// (possibly empty, several, or "merged" when a matching element extends beyond the description).
//
// CONTINUATION. Matching the description is not enough: the logical element (the description without separators)
// can have several separate parts after an edit (the exit loop of a lengthened cross hole, the second breakout line
// of a hole, a piece on the far side of a cut). The picked element is mapped through the edit (each point moved with
// its owners' carriers and projected onto their edited carriers, mapElement) and only the connected parts of the
// logical element that overlap it continue the reference (expect). A reference whose continuation cannot be told is
// counted ("no anchor") and any non-Broken answer to it is not judged; the tests gate that count (each such answer
// must be on the reviewed list tests/naming/oracle/reviewed_unjudged.txt).
// Refinements of the T-081a gate (each has a scripted test, acceptance_test.cpp [oracle-gate] and [t081a-gate]):
//  - An element that holds 90 % of the mapped pick (inside it) continues it, however small a part of it the pick is
//    (the 2 % floor of evalTruth is for overlaps); it runs past the description, so it is merged.
//  - When nothing the picked edge or vertex depends on changed, the one element congruent in place with it is E; a
//    vertex or edge the map leaves in place is anchored within 1e-3 mm (edge: plus the sample chord), not 0.05.
//  - "Merged" means the element took in another element's territory: a part outside the description that lies on
//    the mapped pick, or (nothing moved, or the pick moved with its sweep) on no other baseline element, does not
//    make it merged (ownTerritory). When the pick moved otherwise, a part that lies on no other baseline element
//    where it was, nor where the edit took it (mapElement of each baseline element that moved), is new territory
//    too; a baseline element that might have moved there but cannot be mapped keeps the element merged.
//  - Each face of an edge's or vertex's description needs a face of its own.
//  - A weak reference that moved, whose mapped pick and a mapped sibling piece fall on one element: an Exact answer
//    is not judged (Expectation::mergeUnknown).
//  - A blend owner whose analytic surface cannot be built (Owners::undescribable), or is degenerate and fitted by
//    nothing (Owners::degenerate), leaves the reference without an anchor.
//  - An element that coincides at pick time with another one the description cannot tell from it (coincidesWith
//    Another), and a sliver at pick time, are not references (recordReferences counts them by reason).
//  - A vertex or edge that moved, no part of whose logical element overlaps the mapped pick (a crossing that slid
//    along the face or edge that stayed), is continued (Expectation::crossing): (a) a seam vertex (on only its two
//    faces) by the one seam vertex fitting the description, when it was the only one of its logical element at pick
//    time too; (b) else by the part nearest the mapped pick, when every other part and every other piece's mapped place
//    is at least twice as far from it; else it has no anchor. Both within twice the largest owner shift plus 1 mm.
//
// NEW TERRITORY. An answer that differs from E may still be right when the extra part did not exist at pick time
// (a face that a suppressed cut no longer splits runs on into territory that was nothing). judgeAnswer accepts it
// only when the picked element did not move, the answer holds every expected element, every extra element is
// connected to an answered element that overlaps the mapped pick, and no extra part lies on any element of the
// baseline or on a baseline sibling mapped through the edit. Every acceptance is reported; the tests fail unless it
// is on the reviewed list tests/naming/oracle/reviewed_territory.txt. (New territory on an expected element itself
// is handled by the merged rule below and needs no review.)
//
// WHAT IT CAN MISS (the tests print every bucket; their log is the record of a run):
//  - It shares the script's idea of identity: "the same element" means the same sketch entity, cap, blended edge
//    or imported face. An error both the naming code and the script make (for example two sketch lines swapping
//    places) is invisible.
//  - Which piece of a split logical face or edge is which is decided by separators. When the separating feature
//    itself moves, a separator's verdict far from where it now cuts is not trusted (band rule; strict mode drops it).
//  - Elements whose carriers coincide with another owner's (two logical faces on one plane with the same material
//    side and overlapping extents) cannot be told apart; such faces are described by both owners ("merged") and
//    judged as one. An owner covering under 2 % of a face when it was picked is not part of its description.
//  - Weak references (no separator tells the pieces of the logical element apart: seam-induced pieces, the two
//    pieces either side of a breakout, vertices on fewer than three faces) are judged by position: the expected
//    pieces are those of the continuing parts that overlap the picked element mapped through the edit, or overlap
//    no other baseline piece mapped likewise. Two pieces that both overlap the mapped pick cannot be told apart.
//  - Overlap means 10 % of either element's samples within 0.05 mm of the other and away from its boundary
//    (pieces that only touch do not overlap).
//  - Sampling: a face counts as part of a region from 2 % of its area and as fully inside from 98 %; slivers below
//    2 % are ignored. Edges count from 5 %, vertices from 50 %. Up to 5 % of an element's mesh nodes may lie off
//    its carrier by up to ten times the tolerance (boolean edges are often less precise; strict mode: none). A face
//    the mesher leaves without triangles is sampled on its edges and vertices.
//  - The edit map is a shift of each owner's reference point followed by projection onto the edited carriers (an
//    element whose own carriers did not change moves with the features bounding it, if they moved alike). A sweep
//    edited by a similarity of its profile (move, resize: profileScale) carries its own faces' points exactly, and an
//    element all of whose owners are that sweep's moves with it; an element none of whose carriers changed stays
//    where it is. A rotation of a feature is not modelled (the scripts make none: their edits move, scale, deepen,
//    suppress, insert and reorder). A point that does not settle within 0.01 mm of every carrier is dropped. Where a
//    moved wall meets an unmoved curved face, the map slides along their intersection (such references can end up
//    without an anchor).
//  - Only the carriers listed above. A blend between faces that are not plane/plane or plane/coaxial cylinder has
//    no owner, so its faces cannot be described and are never used as references; scripts avoid them.
//  - Direct edits (T-081b, EditSpec) are modelled on the owners: removed, moved along their normal, or given another
//    radius, with what each removed (or kept) measured from the shapes before and after the edit, never from its
//    history. A face the edit made that lies on no owner (the side of a push/pull slab where a neighbour is not square
//    to the moved face, the step at the bottom of a widened blind hole) is described by nothing, as any new face is.
//    The reach over which faces around a removed face may grow is a curved removed face's smallest box dimension (its
//    hole's diameter, its blend's size), 0.1 mm for a flat one: a face of another owner on the same carrier within
//    that reach would be taken for a continuation only if it also lies where the picked one was or is connected to
//    such a piece. A removal that leaves part of a removed face, or a face run on over it, is not modelled
//    (measureEdit).
//
// Refinements of T-081b (direct edits; each has a replay, random_directedit_test.cpp [oracle-gate]):
//  - A vertex where two pieces of a face met another face, the pieces since joined, nothing moved, continues the pick.
//  - Another element's territory counts (merged) only on the description's own surfaces: the other element must lie on
//    them itself (an element on another surface comes within the tolerance only where the surfaces meet).
//  - Elements of an owner a direct edit removed are gone (not "cannot be mapped, so might lie anywhere").
//  - A piece of the logical element on ground no baseline element held, beside a piece that continues the pick, when
//    nothing moved, is new territory, not part of E (an answer with it is judged by the new-territory rule).
//  - An element merged only because a baseline element that cannot be mapped might have moved there: an Exact answer
//    of exactly E is not judged (Expectation::mergeUnknown).
//  - A weak reference that moved, the pick and another baseline piece mapped onto the same element: no other answer
//    inside the logical element is judged either (Expectation::sharedAnchor).
//  - The logical element gone with its owner while an element still holds all the ground the pick held: no anchor.
// Refinements of T-081b round 12 (identity, docs/naming/POLICY.md; each pinned in random_directedit_test.cpp and
// acceptance_test.cpp): WHOLE GENERATOR and SLID END (expect, oracle.cpp). Elements without identity are judged as
// before.
// RAYCAD_ORACLE_DEBUG_MERGED=1 prints, for every sample that makes an expected element merged, the baseline elements
// it lies on.
//
// STRICT MODE. RAYCAD_ORACLE_STRICT=<list> restores the spike's strictness for the loosenings kept since: "nodes"
// (every node within tolerance), "band" (separators apply everywhere). The tests print which is active. Measured on
// the T-081a review (verdicts per reference compared, RAYCAD_NAMING_VERDICTS): "nodes" changes 0 of 33,785 S4 and
// 5 of 191,745 random verdicts (an existing element judged gone); "band" changes 0 S4 and 130 random verdicts (116
// missed -> gone, 9 correct -> ghost: separators far from where they now cut drop continuations). A third loosening,
// edge and vertex tolerance 2e-3 instead of 1e-3, changed no verdict and was withdrawn.
#pragma once
#include <ray/kernel.h>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <tuple>
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
enum class FType { Import, Sweep, Fillet, Chamfer, Edit };
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
    std::vector<std::array<Vec3, 2>> boundary;      // segments used by one triangle only
    Vec3 boxMin{}, boxMax{};
    void findBoundary();
    // Distance from p to the triangles, if it is at most `limit`; else a value above `limit`. Not `exact`: any value
    // up to limit / 4 may be returned once one is found (enough to say "near").
    double distanceWithin(const Vec3& p, double limit, bool exact = false) const;
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
    // Unique per analyseImport call, for caches of things made from it: an address can be reused once a base is freed
    // (T-081b round 3: a ray grid cached by address served another part's grid in Debug and ASan builds).
    std::uint64_t serial = 0;
};
// Analyses an imported shape once: carrier and fine mesh of every face. KernelFailure if the kernel fails.
Result<std::shared_ptr<const ImportBase>> analyseImport(const kernel::Shape& shape);

// A direct edit (task T-081b: kernel deleteFaces, offsetFaces, resizeHole, unifySameDomain) of the logical faces
// `targets` (owner keys). Its owners (buildOwners) follow from geometry alone, never from the kernel's history:
//  - Delete: the owners in `removed` are gone. The extent of each owner that bordered a removed face (`grown`) also
//    takes in the ground the removed faces left (within each removed face's reach of its mesh as it was: the hole a
//    face grows over when a hole, pocket or blend is removed, the corner a removed fillet rounded).
//  - Offset: each target (a plane with a known outward normal) is moved `amount` along its outward normal (positive
//    adds material, as kernel::offsetFaces), its extent with it. Faces around it are not extended: where they now
//    run on over ground nobody held, the merged rule's new territory covers it (expect).
//  - Resize: each owner in `resized` (the bore: cylinders on one axis and radius) gets radius `amount`, its extent
//    carried radially; the extents of the owners that bordered the bore (`grown`) take in the ring between the two
//    radii (as far as the radius changed from the bore's faces, `fill`) when the bore closes down: where it meets them
//    now.
//  - Unify: no owner changes; a face that unification made of two is described by both owners (merged).
// The measured fields are filled by measureEdit when the edit is built, from the shapes before and after it: which
// owners had a face before and have none after (Delete), which faces of the bore changed radius (Resize). The edit's
// own picks are the faces of its targets (TopoRefs, harness.h).
enum class EKind { Delete, Offset, Resize, Unify };
struct EditSpec {
    EKind kind = EKind::Delete;
    std::vector<std::string> targets;
    double amount = 0;
    std::vector<std::string> removed; // Delete (measured)
    std::vector<std::shared_ptr<const FaceMesh>>
        fill;                         // Delete, Resize (measured): removed / bore faces' meshes, before
    std::vector<double> fillReach;    // parallel to fill: how far a face around may run on over it
    std::vector<std::string> grown;   // Delete, Resize (measured): owners that bordered the removed faces
    std::vector<std::string> resized; // Resize (measured)
    // Per grown owner (measured): the meshes of the other faces on its carrier before the edit. Its grown extent never
    // takes in their ground: that was another element's at pick time (round 3, the reviewer's two-hole case: a hole
    // opened up over a smaller one at the same depth, the floors joined, is a merge, not the floor widening).
    std::map<std::string, std::vector<std::shared_ptr<const FaceMesh>>> held;
};

struct Feature {
    std::string id;
    FType type = FType::Sweep;
    BOp op = BOp::New;
    std::string role; // "import", "extrude", "hole", "fillet", "chamfer", or a direct edit's
    Frame frame;
    std::vector<Loop> loops; // each loop is its own region; Cut extrudes along -n, New and Join along +n
    double dist = 0;
    std::vector<Target> targets;
    double size = 0; // fillet radius, chamfer distance
    bool suppressed = false;
    std::shared_ptr<const ImportBase> base; // Import
    EditSpec edit;                          // Edit
};
using Script = std::vector<Feature>;

// ---------------------------------------------------------------- owners
// What the harness tells the oracle of a built shape beyond its geometry (T-081b round 10): per element (by kind, in
// the shape's order) the names it holds and the names of the faces around it, as opaque strings the oracle compares
// and never reads (no rule of the resolver's is known here).
struct ElementNames {
    std::array<std::vector<std::vector<std::string>>, 3> names, around; // by SubKind (Vertex, Edge, Face)
};
struct Owner {
    std::string key; // "<feature>/s<tag>" side, "<feature>/c<0|1>/<loop>" cap, "<feature>/b<k>" blend,
                     // "<feature>/i<face>" imported face
    Carrier c;
    std::function<bool(const Vec3&, double)> in; // inside the extent, with a margin
    std::string cat;
    // Ground another face on this carrier held before a direct edit that grew this owner's extent (EditSpec::held):
    // an element of the description with any sample there took that face's ground in (larger than described, however
    // little of it). Empty when no edit grew the owner.
    std::function<bool(const Vec3&)> heldByOther;
};
struct Owners {
    std::vector<Owner> v;
    std::map<std::string, int> ix;
    std::map<std::string, Feature> sweeps;    // the sweeps they come from, by id (mapElement: moved features)
    std::shared_ptr<const ImportBase> import; // the imported base shape, if any (mapElement: material that stays)
    std::uint64_t serial = 0;                 // unique per buildOwners call (a cache key)
    // Keys of logical faces the script has but the oracle cannot describe: a blend whose analytic surface does not
    // exist for its parents (a fillet larger than the corner arc it rounds, a torus with its tube wider than its ring).
    // A reference described by such an owner has no continuation the oracle can tell (expect: noAnchor).
    std::set<std::string> undescribable;
    // Keys of blends whose analytic surface is degenerate (a torus whose tube is as wide as its ring: a fillet at least
    // as large as the corner arc it rounds). The kernel may build that surface or another one; where no element of the
    // edited shape fits a description using such an owner, its continuation cannot be told (expect: noAnchor).
    std::set<std::string> degenerate;
    // A direct edit changed an imported face (Feature::edit): the imported base solid is no longer material that stays
    // where it was (mapElement).
    bool importEdited = false;
    // Owners a direct edit removed (EditSpec::removed): their elements are gone, not somewhere the oracle cannot map
    // them to (TruthBuilder::mappedTo).
    std::set<std::string> removedByEdit;
    // Every owner a direct edit removed, moved or resized (expect: a separator that only a direct edit moved).
    std::set<std::string> touchedByEdit;
    // Every owner whose extent a direct edit's fill grew (EditSpec::grown; expect: ground only an edit gave an owner).
    std::set<std::string> grownByEdit;
    // Set by the harness for a replay (T-081b round 10): the features of the edited script the replay did not build
    // (harness.h builtOnly leaves their owners out, and with them what they did to the other owners' extents), and the
    // names of the built shape's elements (expect: IN PLACE BY NAME). Empty / null otherwise.
    std::set<std::string> dropped;
    std::shared_ptr<const ElementNames> names;
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
    bool sided = true;       // planar face: false when the sign of nrm is unknown (a face without triangles)
    int sigma = 0;           // cylindrical face: +1 boss, -1 hole
    Vec3 boxMin{}, boxMax{};
    std::shared_ptr<const FaceMesh> mesh; // face: its triangles
    // The answer signature's data (harness.h answerSignature, T-081b round 6): the centroid by the samples' weights
    // (an edge without samples: its curve's midpoint by the kernel), the area or length (an edge: the kernel's
    // length), an edge's end points (the kernel's, sorted), and whether another element of its kind in the shape has a
    // signature that cannot be told from this one's (a near twin: within 0.02 mm and 1 %).
    Vec3 centroid{};
    double measure = 0;
    Vec3 end0{}, end1{};
    bool twin = false;
};
struct Samples {
    std::array<std::vector<ElemSamples>, 3> e;                      // by SubKind (Vertex, Edge, Face)
    std::array<std::vector<std::vector<std::uint32_t>>, 2> facesOf; // [0] faces of each vertex, [1] of each edge
    std::vector<std::vector<std::uint32_t>> verticesOf;             // vertices of each edge
    std::uint32_t unmeshedFaces = 0; // faces without triangles, sampled on their edges (material side unknown)
    const std::vector<ElemSamples>& of(SubKind k) const { return e[static_cast<std::size_t>(k)]; }
};
Result<std::shared_ptr<const Samples>> sampleShape(const kernel::Shape& shape);

// The strictness knobs in force (RAYCAD_ORACLE_STRICT), in words.
std::string strictnessName();

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
    std::vector<std::uint32_t> elems;   // sorted
    bool merged = false;                // a matching element extends beyond the description
    std::vector<std::uint32_t> partial; // the elements that do (sorted)
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
    // Element s mapped through the edit to `after` (mapElement with its own description), made once per element and
    // edited script (Owners::serial); thread-safe. nullopt: it cannot be described or mapped; no samples: it did not
    // move.
    const std::optional<ElemSamples>& mappedTo(SubId s, const Owners& after) const;

private:
    const Owners& owners_;
    const Samples& samples_;
    std::vector<TruthDesc> faceDesc_;
    mutable std::mutex lock_;
    mutable std::map<std::tuple<std::uint64_t, int, std::uint32_t>, std::optional<ElemSamples>> mapped_;
};

// Does another element of the same kind lie where element `s` lies (they share territory, oracle.h overlap) and fit
// its description without separators too? Then nothing the oracle knows tells the two apart: a boolean that leaves two
// coincident edges along a zero-height wall, or back-to-back faces of the same material side. Such an element is not
// a reference (TruthBuilder::describe says why), as a pick the oracle cannot vouch for is not.
bool coincidesWithAnother(const TruthDesc& d, SubId s, const Owners& owners, const Samples& samples);

// The edges of the shape on the logical edge `target`, each with its own description.
std::vector<std::pair<std::uint32_t, TruthDesc>> pickTarget(const Target& target, const Owners& owners,
                                                            const Samples& samples);

// The faces of the shape that lie on owner `key` (its carrier, type and material side, inside its extent, from 2 % of
// their area), by geometry alone; empty if the owner does not exist.
std::vector<std::uint32_t> facesOn(const std::string& key, const Owners& owners, const Samples& samples);

// Fills the measured fields of direct edit `f` (EditSpec) from the shape before it (`before`, built from the script
// `built` of the features before it) and after it. Geometry only. Returns why the oracle cannot describe the result
// (a picked face still there, a face away from the picked ones gone, a bore face not at its new radius, a moved face
// not where it was moved to), or an empty string.
std::string measureEdit(Feature& f, const Script& built, const Samples& before, const Samples& after);

// Unjudged: a non-Broken answer to a reference whose continuation the oracle cannot tell (gated by the tests).
enum class Verdict { Correct, BrokenGone, Missed, Unjudged, WrongOther, WrongIncomplete, WrongGhost, WrongSilentMerge };
constexpr std::size_t kVerdicts = 8;
const char* verdictName(Verdict v);
inline bool isWrong(Verdict v)
{
    return v >= Verdict::WrongOther;
}
// The answer of a resolver, in the oracle's terms.
enum class Answer { Exact, Split, Merged, Repaired, Broken };
// `answer` with `elements` against the expected answer `te`, by set only (the first step of judgeAnswer).
Verdict judge(Answer answer, std::vector<std::uint32_t> elements, const TruthEval& te);

// Elements of `samples` whose nodes all lie within `tol` of the picked element's nodes and vice versa
// (congruent in place). Independent of the description; a cross-check of it.
std::vector<std::uint32_t> congruent(const ElemSamples& picked, SubKind kind, const Samples& samples, double tol);

// ---------------------------------------------------------------- the full judge
// What the oracle keeps of a reference from pick time.
struct PickRecord {
    SubId base;                              // the element picked
    TruthDesc truth;                         // its description
    std::shared_ptr<const Samples> baseline; // samples of the shape it was picked on
    std::shared_ptr<const Owners> owners;    // owners of the script it was picked on
    // Optional: describes the other baseline elements (built on `owners` and `baseline`, which it must not outlive),
    // so the merged rule can map them through the edit (expect). Without it a moved element that runs past its
    // description is merged, as before.
    std::shared_ptr<const TruthBuilder> builder;
    // Optional (T-081b round 10): the names the picked element held and those of the faces around it at pick time, as
    // ElementNames has them (empty: not known; expect's IN PLACE BY NAME then does not apply).
    std::vector<std::string> names, around;
    const ElemSamples& picked() const { return baseline->of(base.kind)[base.index]; }
};

// Element `e` of `baseline` moved through the edit: every point shifted with its owners' carriers (the largest
// shift of the reference points of the owners it lies on) and projected onto their carriers in `after`. A point
// inside the volume of a sweep that only moved moves with that sweep (the part of a hole wall inside a moved boss).
// When its own carriers did not change, it moves with the features that bound it instead (the owners its vertices
// lie on besides its own), if they all moved alike: a piece of an edge between two cuts moves with them. Points that
// do not settle
// on all its carriers are dropped; if under half the weight maps, the result has no samples. `moved` is set when a
// carrier of the element or of one of its separators changed, or its bounding features moved.
ElemSamples mapElement(const Samples& baseline, SubId e, const TruthDesc& d, const Owners& before, const Owners& after,
                       bool& moved);

// Share (by weight) of a's samples lying on element b (within 0.05 mm; 1e-3 mm for vertices).
double shareOnElement(const ElemSamples& a, const ElemSamples& b, SubKind kind);

// A sliver: a face or edge with no sample more than 0.02 mm inside it (a corner left by a cut). Which piece holds a
// sliver cannot be told by position; judgeAnswer leaves slivers out of both the answer and the expectation (an
// answer of slivers alone is wrong).
bool isSliver(const ElemSamples& es, SubKind kind);

// The connected components of `elems` (faces through shared edges, edges through shared vertices; a vertex is its
// own): for each element, the index of its component.
std::vector<int> componentsOf(const std::vector<std::uint32_t>& elems, SubKind kind, const Samples& samples);

// The expectation for one reference on an edited shape, computed once; judgeAnswer then judges any answer.
struct Expectation {
    SubKind kind = SubKind::Face;
    bool weak = false;
    TruthEval strict;                   // elements fitting the description, separators included
    std::vector<std::uint32_t> logical; // elements fitting it without separators (sorted; includes strict)
    std::vector<int> component;         // per element of `logical`
    std::vector<char> anchored;         // per element of `logical`: overlaps the mapped pick
    std::vector<char> claimed;          // weak only, per element of `logical`: overlaps another baseline piece of the
                                        // logical element, mapped through the edit (that piece's continuation)
    int siblings = 0;                   // weak only: how many other pieces the logical element had at pick time
    bool twins = false;                 // E is two or more vertices at one point (a boolean leaves coincident
                                        // vertices): position cannot tell them apart, and any one is an Exact answer
    ElemSamples mapped;                 // the picked element mapped through the edit
    bool moved = false;
    TruthEval expected;       // E: the strict elements in components that continue the pick
    std::string noAnchor;     // why the continuation cannot be told (then E is not trusted)
    std::string crossing;     // non-empty: E was told by the sliding-crossing rule (expect, oracle.cpp), and how
    long congruentIndex = -1; // the one element congruent in place with the pick, or -1
    // A weak reference that moved, whose mapped pick and another piece's mapped position fall on one element of E:
    // whether that element continues both (merged) cannot be told, so an Exact answer is not judged (a Merged or Split
    // answer of exactly E is right either way).
    bool mergeUnknown = false;
    // The same case: E is where both the pick and another baseline piece of its logical element went, so which of the
    // logical element's parts continues the pick cannot be told either: any other answer inside the logical element is
    // not judged (T-081b directedit 6:3: a cut moved across a hole, its wall's two crossings with the hole wall).
    bool sharedAnchor = false;
    // The pieces were matched sibling to sibling (expect, round 3): E is the one piece mutually nearest the mapped
    // pick.
    bool siblingMatch = false;
    // An expected edge past the mapped pick's ends was dropped (expect, round 3: a moved edge's old end).
    bool pastMappedEnd = false;
    // The one expected face held none of the picked ground, in place or mapped (the structural check, expect): the
    // pick is gone, E emptied.
    bool structuralGone = false;
    // Expected "gone", but an element lies where the pick was, every owner of the description on its carrier still:
    // E is that element (expect: HELD IN PLACE, T-081b round 5).
    bool heldInPlace = false;
    // Expected "gone" on a replay that dropped a feature, though an element lies where the pick was with the pick's
    // names and the names of the faces it had around it: E is that element (expect: IN PLACE BY NAME, T-081b round 10).
    bool inPlaceByName = false;
    // Pieces holding none of the pick on ground nobody held were left out of E (expect: NEW PIECES ARE NEVER REQUIRED).
    bool newPiecesDropped = false;
    // The map takes every sample of the pick out of the edited part (outside the box of its faces): the pick's image
    // left the part (expect; holdsPickedGround then does not credit the pick's old place: T-081b round 7, the
    // reviewer's v3).
    bool imageOutside = false;
    // Every carrier of the description is as it was at pick time (only separators or bounding features moved): the
    // picked element's ground is where it was (T-081b round 2: remnants, and the new-ground rule's coverage).
    bool inPlace = false;
    // E was told by identity (T-081b round 12, docs/naming/POLICY.md; expect, oracle.cpp): WHOLE GENERATOR (the pick
    // was the one face of its generator, and the edited generator has one face: that face, wherever it lies; the
    // structural check does not apply) or SLID END (an edge between the same end elements, its other end a two-face
    // vertex that slid along the curve).
    bool wholeGenerator = false;
    bool slidEnd = false;
    bool inLogical(std::uint32_t e) const;
    bool isAnchored(std::uint32_t e) const;
    bool isClaimed(std::uint32_t e) const;
    int componentOf(std::uint32_t e) const; // -1 if not in `logical`
};
Expectation expect(const PickRecord& pick, const Owners& now, const Samples& samples);

struct Judgement {
    Verdict verdict = Verdict::Correct;
    bool unjudged = false;  // a non-Broken answer to a reference whose continuation cannot be told
    std::string accepted;   // non-empty: accepted by the new-territory rule (why, in words)
    std::string newGround;  // non-empty: a flagged answer accepted by the new-ground rule (judgeAnswer; why, in words)
    bool twin = false;      // Exact on one of coincident vertices (Expectation::twins)
    std::string structural; // non-empty: WRONG by the structural check (judgeAnswer; why, in words)
};
Judgement judgeAnswer(const Expectation& e, const PickRecord& pick, const Owners& now, const Samples& samples,
                      Answer answer, std::vector<std::uint32_t> elements);

} // namespace ray::naming_oracle
