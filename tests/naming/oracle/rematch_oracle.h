// The rematch oracle (task T-084): judges, from geometry alone, what a reference to an element of an imported body
// should resolve to after the body's STEP file was replaced by a new revision (naming::rematchImported).
//
// INDEPENDENCE. Like oracle.h, this links ray_kernel and ray_core only and includes no naming header: it never sees a
// name, a fingerprint or a territory of the naming code. It reads the two shapes through public kernel queries
// (tessellate, surfaceInfo, curveInfo, vertexPoint, adjacent) and its own point-to-mesh distances.
//
// IDENTITY. Two elements of the same kind are IDENTICAL when every sample of each lies within kTolerance of the
// other: for faces, the mesh nodes and the interior samples (triangle centres) of each face against the other face's
// triangles (and the same surface type); for
// edges, the polyline points of each against the other's polyline; for vertices, the points. Two elements SHARE GROUND
// when an interior sample of one (kMargin from its own boundary) lies on the other. The tolerance is the mesh chord
// plus 1e-3 mm, an order above rematch's 1e-3 mm, so an element rematch calls unchanged is always identical here, and an
// element this oracle calls different differs by more than rematch's tolerance.
//
// JUDGING one reference (the old element `pick`, the resolver's state and answer in the new shape):
//  - Exact: right only when the answer is one element identical to the pick. Anything else is WRONG (a silent wrong
//    match: the consumer uses it without review).
//  - Repaired (a suggestion): right only when identical; otherwise WRONG (S4 counted wrong suggestions as wrong).
//  - Split, Merged (flagged: review needed): every answered element must share ground with the pick, and every element
//    holding at least kMissingShare of the pick's interior samples must be in the answer; otherwise WRONG.
//  - Broken: never wrong. LOST when an identical element exists (the reference could have been kept), FLAGGED when some
//    element shares ground with the pick (it changed), REMOVED when nothing does.
#pragma once
#include <ray/kernel.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ray::rematch_oracle {

using kernel::SubId;
using kernel::SubKind;

inline constexpr double kChord = 0.004;               // mesh chord, mm
inline constexpr double kTolerance = 2 * kChord + 1e-3; // on the other element, mm
inline constexpr double kMargin = 3 * kTolerance;       // interior samples, from the element's own boundary, mm
inline constexpr double kMissingShare = 0.2;            // of the pick's interior samples

// The samples of every element of one shape.
class Samples {
public:
    explicit Samples(const kernel::Shape& shape);
    ~Samples();
    Samples(const Samples&) = delete;
    Samples& operator=(const Samples&) = delete;

    const kernel::Shape& shape() const;
    bool ok() const; // the shape was meshed
    std::uint32_t count(SubKind kind) const;

    // Is p within `tol` of the element?
    bool on(SubId element, const Vec3& p, double tol = kTolerance) const;
    bool identical(SubId mine, const Samples& other, SubId theirs) const;
    // The share of `mine`'s interior samples that lie on `theirs` (0 when it has none).
    double shareOn(SubId mine, const Samples& other, SubId theirs) const;
    bool sharesGround(SubId mine, const Samples& other, SubId theirs) const;
    // Elements of `other` of the pick's kind that are identical to it / share ground with it.
    std::vector<SubId> identicalIn(SubId mine, const Samples& other) const;
    std::vector<SubId> sharingIn(SubId mine, const Samples& other) const;
    // A point on the face at least `clearance` from its boundary (the most central interior sample), if any.
    std::optional<Vec3> interiorPoint(SubId face, double clearance) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class State { Exact, Split, Merged, Repaired, Broken };
enum class Verdict { Matched, Flagged, Removed, Lost, Wrong };
struct Judgement {
    Verdict verdict = Verdict::Wrong;
    std::string why;
};
Judgement judge(const Samples& before, SubId pick, const Samples& after, State state,
                const std::vector<SubId>& answer);

} // namespace ray::rematch_oracle
