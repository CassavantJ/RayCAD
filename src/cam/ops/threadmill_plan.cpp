// The thread milling operation's plan (threadmill.h, task T-056): parameters, checks, the features and their thread
// data, the passes and the linked moves. Every rule is stated in threadmill.h; this file follows them in its order.
// Deterministic: integer nanometres after toNm, dmath for the trigonometry, holes from recognizeHoles (sorted) and
// orderHolesShortestPath, geom2d and link.
#include "drill.h" // isLeftHandThread, kHolderClearanceMm, kThreadPitchTolNm, kThreadDiameterTolMm, kDiameterTolMm
#include "op_params.h"
#include "threadmill.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <utility>

// Returns the error of a Status or Result expression.
#define RAY_TRY_STATUS(expr)                                                                                           \
    do {                                                                                                               \
        if (auto ray_thread_status_ = (expr); !ray_thread_status_) {                                                   \
            return tl::unexpected(std::move(ray_thread_status_.error()));                                              \
        }                                                                                                              \
    } while (false)

namespace ray::cam::ops {

using geom2d::Coord;
using geom2d::Regions;

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kInternalDepthPerPitch = 0.54126587736527421; // 5/8 H, H = sqrt(3) / 2 P (ISO 68-1)
constexpr double kExternalDepthPerPitch = 0.61343474938897728; // 17/24 H

tl::unexpected<Error> failure(ErrorCode code, std::string message)
{
    return tl::unexpected(Error{code, std::move(message), {}});
}
tl::unexpected<Error> invalid(std::string message)
{
    return failure(ErrorCode::InvalidArgument, std::move(message));
}

// A length in mm for messages: rounded to 1 um (half to even), trailing zeros dropped ("12.5", "0.002", "-3").
std::string mmText(Nm nm)
{
    std::int64_t um = nm / 1000;
    std::int64_t rest = nm % 1000;
    if (rest < 0) {
        rest += 1000;
        --um;
    }
    if (rest > 500 || (rest == 500 && (um % 2) != 0)) {
        ++um;
    }
    const bool negative = um < 0;
    const std::uint64_t a = negative ? static_cast<std::uint64_t>(-(um + 1)) + 1 : static_cast<std::uint64_t>(um);
    std::string out = (negative ? "-" : "") + std::to_string(a / 1000);
    std::uint64_t frac = a % 1000;
    if (frac != 0) {
        std::string digits = std::to_string(frac);
        digits.insert(0, 3 - digits.size(), '0');
        while (!digits.empty() && digits.back() == '0') {
            digits.pop_back();
        }
        out += "." + digits;
    }
    return out;
}
std::string mmText(double mm)
{
    return std::isfinite(mm) && std::abs(mm) < 1e12 ? mmText(toNm(mm)) : std::string("?");
}

// a / b rounded half to even, b > 0.
std::int64_t divHalfEven(std::int64_t a, std::int64_t b)
{
    std::int64_t q = a / b;
    std::int64_t r = a % b;
    if (r < 0) {
        r += b;
        --q;
    }
    if (2 * r > b || (2 * r == b && (q % 2) != 0)) {
        ++q;
    }
    return q;
}

std::int64_t roundHalfEven(double v)
{
    return static_cast<std::int64_t>(std::nearbyint(v));
}

Vec3 apply(const Xform& f, Vec3 p)
{
    const double* m = f.m;
    return Vec3{m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}
Vec3 rotate(const Xform& f, Vec3 v)
{
    const double* m = f.m;
    return Vec3{m[0] * v.x + m[1] * v.y + m[2] * v.z, m[4] * v.x + m[5] * v.y + m[6] * v.z,
                m[8] * v.x + m[9] * v.y + m[10] * v.z};
}

struct Params {
    bool internal = true;
    bool multi = false;
    double pitch = 0, diameter = 0, length = 0, depth = 0;
    bool leftHand = false;
    bool climb = true;
    int radialPasses = 1;
    bool springPass = false;
    double overlap = 0;
    bool edgeFeed = true;
};

const OpSchema* threadmillSchema()
{
    // Parsed once; immutable afterwards, so workers share it without a lock.
    static const std::shared_ptr<const OpSchema> schema = [] {
        auto loaded = OpSchema::load(embeddedOpSchema("threadmill"));
        return loaded ? *loaded : std::shared_ptr<const OpSchema>();
    }();
    return schema.get();
}

Result<Params> readParams(std::string_view paramsJson)
{
    const OpSchema* schema = threadmillSchema();
    if (schema == nullptr) {
        return failure(ErrorCode::Internal, "The thread milling operation's parameter schema does not load");
    }
    auto p = schema->read(paramsJson);
    if (!p) {
        return tl::unexpected(p.error());
    }
    Params out;
    out.internal = p->text("location") != "external";
    out.multi = p->text("form") == "multi";
    out.pitch = p->number("pitch");
    out.diameter = p->number("diameter");
    out.length = p->number("threadLength");
    out.depth = p->number("threadDepth");
    out.leftHand = p->flag("leftHand");
    out.climb = p->text("direction") != "conventional";
    out.radialPasses = static_cast<int>(p->number("radialPasses"));
    out.springPass = p->flag("springPass");
    out.overlap = p->number("overlap");
    out.edgeFeed = p->flag("edgeFeed");
    return out;
}

// A feature found from a picked face, before its thread data.
struct Feature {
    bool internal = true;
    Nm x = 0, y = 0;
    Nm diameter = 0;   // the drilled bore, or the cylinder
    Nm top = 0;        // the bore's top, or the cylinder's
    Nm bottom = 0;     // the bore's bottom, or the cylinder's
    Nm featureTop = 0; // the hole's top (above a counterbore), or the cylinder's
    Nm freeBottom = 0; // internal: the drilled hole is free above this
    bool through = false;
    const RecognizedHole* hole = nullptr;
    std::size_t part = 0;
};

class Planner {
public:
    Planner(const GenerateInput& in, JobContext& ctx) : in_(in), ctx_(ctx) {}

    Result<ThreadmillPlan> run()
    {
        auto params = readParams(in_.op.paramsJson);
        if (!params) {
            return tl::unexpected(params.error());
        }
        p_ = *params;
        plan_.multiForm = p_.multi;
        plan_.overlapDeg = p_.multi ? p_.overlap : 0;
        RAY_TRY_STATUS(checkTool());
        RAY_TRY_STATUS(checkCutting());
        RAY_TRY_STATUS(checkHeights());
        RAY_TRY_STATUS(findFeatures());
        for (const Feature& f : features_) {
            RAY_TRY_STATUS(ctx_.cancel.check());
            RAY_TRY_STATUS(threadOf(f));
        }
        RAY_TRY_STATUS(levels());
        RAY_TRY_STATUS(link());
        return std::move(plan_);
    }

private:
    const lib::Tool& tool() const { return in_.tool; }

    Status checkTool()
    {
        const lib::Tool& t = tool();
        if (t.type != lib::ToolType::ThreadMill) {
            return invalid("Thread milling needs a thread mill; the tool is not one (choose a thread mill)");
        }
        if (!(t.diameter > 0) || !std::isfinite(t.diameter)) {
            return invalid("The thread mill has no diameter");
        }
        if (p_.multi) {
            if (!(t.threadPitch > 0)) {
                return invalid("Multi-form thread milling needs a full-form thread mill with the thread's pitch; this "
                               "thread mill has none (it is single-form): choose single-form, or a multi-form tool");
            }
            if (!(t.fluteLength > 0)) {
                return invalid("The multi-form thread mill has no flute length: set its toothed length in the tool "
                               "library (each step of the thread is that long)");
            }
            const Nm pitch = toNm(t.threadPitch);
            plan_.teeth = toNm(t.fluteLength) / std::max<Nm>(1, pitch);
            if (plan_.teeth < 1) {
                return invalid("The thread mill's flutes (" + mmText(t.fluteLength) +
                               " mm) are shorter than one pitch (" + mmText(t.threadPitch) + " mm)");
            }
        }
        return {};
    }

    Status checkCutting()
    {
        const CuttingData& c = in_.op.cutting;
        const double rpm = c.spindleRpm;
        if (!(rpm > 0) || !std::isfinite(rpm)) {
            return invalid("Set a spindle speed");
        }
        if (!c.clockwise) {
            return invalid("The spindle is set to turn counter-clockwise (M4): a right-hand thread mill does not cut "
                           "that way; set it clockwise (M3) and choose the hand of the thread and climb or "
                           "conventional milling: the direction of travel follows from them");
        }
        if (in_.machine.maxRpm > 0 && rpm > in_.machine.maxRpm) {
            return invalid("The spindle speed " + mmText(rpm) + " rpm is above the machine's maximum of " +
                           mmText(in_.machine.maxRpm) + " rpm");
        }
        if (in_.machine.minRpm > 0 && rpm < in_.machine.minRpm) {
            return invalid("The spindle speed " + mmText(rpm) + " rpm is below the machine's minimum of " +
                           mmText(in_.machine.minRpm) + " rpm");
        }
        constexpr double kMaxFeed = static_cast<double>(std::numeric_limits<std::int32_t>::max());
        const auto feedOf = [&](double mmPerMin) -> std::int32_t {
            const double f = mmPerMin * 1000.0;
            return mmPerMin > 0 && std::isfinite(f) && f <= kMaxFeed && roundHalfEven(f) >= 1
                       ? static_cast<std::int32_t>(roundHalfEven(f))
                       : 0;
        };
        feed_ = feedOf(c.feedCut);
        if (feed_ == 0) {
            return invalid("Set a cutting feed: it is the feed along the thread");
        }
        plunge_ = feedOf(c.feedPlunge);
        leadIn_ = feedOf(c.feedLeadIn);
        leadOut_ = feedOf(c.feedLeadOut);
        return {};
    }

    Status checkHeights()
    {
        const Heights& h = in_.op.heights;
        for (const auto& [v, word] : {std::pair{h.clearance, "clearance"}, std::pair{h.retract, "retract"},
                                      std::pair{h.feedPlane, "feed plane"}}) {
            if (!std::isfinite(v) || std::abs(v) > 1e6) {
                return invalid(std::string("The ") + word + " height is not a usable number");
            }
        }
        if (h.feedPlane < 0) {
            return invalid("The feed plane (" + mmText(h.feedPlane) +
                           " from the highest top of the holes and bosses) must not be below that top");
        }
        if (h.retract < h.feedPlane) {
            return invalid("The retract height (" + mmText(h.retract) + ") must not be below the feed plane (" +
                           mmText(h.feedPlane) + ")");
        }
        if (h.clearance < h.retract) {
            return invalid("The clearance height (" + mmText(h.clearance) + ") must not be below the retract height (" +
                           mmText(h.retract) + ")");
        }
        return {};
    }

    Result<const std::vector<RecognizedHole>*> holesOf(std::size_t part)
    {
        if (const auto it = recognised_.find(part); it != recognised_.end()) {
            return &it->second;
        }
        const HoleOptions options = holeOptionsFor(in_.geom, part);
        auto found = recognizeHoles(in_.geom.partShapes[part], in_.geom.wcsFromModel, ctx_, options);
        if (!found) {
            return tl::unexpected(found.error());
        }
        return &recognised_.emplace(part, std::move(*found)).first->second;
    }

    std::string selectedWords(std::size_t i) const
    {
        const kernel::SubId sub = in_.geom.selected[i];
        const std::size_t part = static_cast<std::size_t>(in_.geom.selectedShapeIndex[i]);
        std::string text = "Selection " + std::to_string(i + 1);
        if (sub.kind == kernel::SubKind::Face) {
            auto s = kernel::surfaceInfo(in_.geom.partShapes[part], sub);
            if (s) {
                const Vec3 c = apply(in_.geom.wcsFromModel, s->centroid);
                text += " (the face centred at X" + mmText(c.x) + " Y" + mmText(c.y) + " Z" + mmText(c.z) + ")";
            }
        }
        return text;
    }

    Status findFeatures()
    {
        const ResolvedGeometry& g = in_.geom;
        if (g.selected.empty()) {
            return invalid(p_.internal ? "Pick the holes to thread: select the wall of each hole"
                                       : "Pick the bosses to thread: select the cylinder of each boss or pin");
        }
        if (g.selectedShapeIndex.size() != g.selected.size()) {
            return failure(ErrorCode::Internal, "The selection and its part indices differ in length");
        }
        std::set<std::pair<std::size_t, std::size_t>> takenHoles;
        std::set<std::pair<Nm, Nm>> takenBosses;
        for (std::size_t i = 0; i < g.selected.size(); ++i) {
            RAY_TRY_STATUS(ctx_.cancel.check());
            const kernel::SubId sub = g.selected[i];
            const int partIndex = g.selectedShapeIndex[i];
            if (partIndex < 0 || static_cast<std::size_t>(partIndex) >= g.partShapes.size()) {
                return failure(ErrorCode::Internal, "A selection lies on no part body");
            }
            const std::size_t part = static_cast<std::size_t>(partIndex);
            if (sub.kind != kernel::SubKind::Face) {
                return invalid(selectedWords(i) + " is not a face: pick " +
                               (p_.internal ? "the wall of each hole" : "the cylinder of each boss"));
            }
            if (p_.internal) {
                auto holes = holesOf(part);
                if (!holes) {
                    return tl::unexpected(holes.error());
                }
                std::optional<std::size_t> found;
                for (std::size_t k = 0; k < (*holes)->size() && !found; ++k) {
                    const auto& faces = (**holes)[k].faces;
                    if (std::binary_search(faces.begin(), faces.end(), sub)) {
                        found = k;
                    }
                }
                if (!found) {
                    return failure(ErrorCode::ToolpathFailed,
                                   selectedWords(i) +
                                       " is not part of a hole that can be threaded from this setup: a hole must run "
                                       "along the setup's Z and be open from above (for an outside thread, choose "
                                       "External)");
                }
                if (!takenHoles.insert({part, *found}).second) {
                    continue;
                }
                const RecognizedHole& h = (**holes)[*found];
                const HoleSegment& bore = h.segments[h.bore];
                Feature f;
                f.internal = true;
                f.x = h.x;
                f.y = h.y;
                f.diameter = toNm(bore.diameter);
                f.top = bore.zTop;
                f.bottom = bore.zBottom;
                f.featureTop = h.zTop;
                f.through = h.bottom == HoleBottom::Through;
                f.freeBottom = f.through ? h.zBottom : bore.zBottom;
                f.hole = &h;
                f.part = part;
                features_.push_back(f);
            } else {
                auto f = boss(part, sub, i);
                if (!f) {
                    return tl::unexpected(f.error());
                }
                if (takenBosses.insert({f->x, f->y}).second) {
                    features_.push_back(*f);
                }
            }
        }
        if (features_.size() > 1) {
            std::vector<Vec2> xy;
            xy.reserve(features_.size());
            for (const Feature& f : features_) {
                xy.push_back(Vec2{toMm(f.x), toMm(f.y)});
            }
            const std::vector<std::size_t> order = orderHolesShortestPath(xy, Vec2{0, 0});
            std::vector<Feature> ordered;
            ordered.reserve(order.size());
            for (const std::size_t k : order) {
                ordered.push_back(features_[k]);
            }
            features_ = std::move(ordered);
        }
        return {};
    }

    // An external thread's cylinder.
    Result<Feature> boss(std::size_t part, kernel::SubId face, std::size_t i)
    {
        const kernel::Shape& shape = in_.geom.partShapes[part];
        const Xform& frame = in_.geom.wcsFromModel;
        auto s = kernel::surfaceInfo(shape, face);
        if (!s) {
            return tl::unexpected(s.error());
        }
        const Vec3 axis = rotate(frame, s->axis);
        if (s->type != kernel::SurfaceType::Cylinder || std::abs(std::abs(axis.z) - 1) > 1e-9) {
            return failure(ErrorCode::ToolpathFailed, selectedWords(i) +
                                                          " is not a cylinder along the setup's Z: pick the "
                                                          "cylindrical face of a boss or pin (for a hole, choose "
                                                          "Internal)");
        }
        auto sample = kernel::faceSample(shape, face);
        if (!sample) {
            return tl::unexpected(sample.error());
        }
        // Material inside the cylinder: the face's outward normal points away from the axis.
        const Vec3 d{sample->point.x - s->origin.x, sample->point.y - s->origin.y, sample->point.z - s->origin.z};
        const double along = d.x * s->axis.x + d.y * s->axis.y + d.z * s->axis.z;
        const Vec3 radial{d.x - along * s->axis.x, d.y - along * s->axis.y, d.z - along * s->axis.z};
        const double out = radial.x * sample->normal.x + radial.y * sample->normal.y + radial.z * sample->normal.z;
        if (!(out > 0)) {
            return failure(ErrorCode::ToolpathFailed,
                           selectedWords(i) + " is the wall of a hole, not of a boss: choose Internal for a hole");
        }
        Feature f;
        f.internal = false;
        const Vec3 c = apply(frame, s->origin);
        f.x = toNm(c.x);
        f.y = toNm(c.y);
        f.diameter = toNm(2 * s->radius);
        double zMin = std::numeric_limits<double>::infinity(), zMax = -zMin;
        for (const kernel::SubId e : kernel::adjacent(shape, face, kernel::SubKind::Edge)) {
            auto ec = kernel::edgeCurves(shape, e, frame);
            if (!ec) {
                return tl::unexpected(ec.error());
            }
            zMin = std::min(zMin, ec->zMin);
            zMax = std::max(zMax, ec->zMax);
        }
        if (!(zMax > zMin)) {
            return failure(ErrorCode::KernelFailure, selectedWords(i) + ": the cylinder's height cannot be read");
        }
        f.top = toNm(zMax);
        f.bottom = toNm(zMin);
        f.featureTop = f.top;
        f.freeBottom = f.bottom;
        f.part = part;
        return f;
    }

    std::string whereOf(const Feature& f) const
    {
        return std::string(f.internal ? "the hole" : "the boss") + " at X" + mmText(f.x) + " Y" + mmText(f.y);
    }

    Status threadOf(const Feature& f)
    {
        const lib::Tool& t = tool();
        PlannedThread th;
        th.internal = f.internal;
        th.x = f.x;
        th.y = f.y;
        th.top = f.top;
        th.featureTop = f.featureTop;
        th.boreDiameter = f.diameter;
        th.boreBottom = f.freeBottom;
        th.where = whereOf(f);
        const std::string& at = th.where;
        const RecognizedHole* h = f.hole;
        const bool spec = h != nullptr && !h->threadSpec.empty();
        // Pitch.
        const Nm holePitch = spec ? toNm(h->threadPitch) : 0;
        if (p_.pitch > 0) {
            th.pitch = toNm(p_.pitch);
            if (holePitch > 0 && std::abs(th.pitch - holePitch) > kThreadPitchTolNm) {
                return invalid("The pitch entered (" + mmText(th.pitch) + " mm) does not match the " + h->threadSpec +
                               " thread of " + at + " (pitch " + mmText(holePitch) + " mm)");
            }
        } else if (holePitch > 0) {
            th.pitch = holePitch;
        } else {
            return invalid(f.internal
                               ? "The model gives no thread for " + at +
                                     " (an imported part, or a hole not made as tapped) and no pitch is entered: "
                                     "enter the pitch and the major diameter"
                               : "Enter the pitch of the thread on " + at +
                                     ": the model has no thread data for "
                                     "bosses");
        }
        if (th.pitch <= 0) {
            return invalid("The pitch must be more than 0");
        }
        if (t.threadPitch > 0 && std::abs(toNm(t.threadPitch) - th.pitch) > kThreadPitchTolNm) {
            return invalid("The thread mill's pitch (" + mmText(t.threadPitch) + " mm) does not match the thread's (" +
                           mmText(th.pitch) + " mm) at " + at);
        }
        // Major diameter.
        const double holeD = spec ? h->threadNominalDiameter : 0;
        if (p_.diameter > 0) {
            th.majorDiameter = toNm(p_.diameter);
            if (holeD > 0 && std::abs(p_.diameter - holeD) > kThreadDiameterTolMm) {
                return invalid("The diameter entered (" + mmText(p_.diameter) + " mm) does not match the " +
                               h->threadSpec + " thread of " + at + " (" + mmText(holeD) + " mm)");
            }
        } else if (holeD > 0) {
            th.majorDiameter = toNm(holeD);
        } else if (!f.internal) {
            th.majorDiameter = f.diameter;
        } else {
            return invalid("The model gives no major diameter for the thread of " + at +
                           ": enter the thread's major (nominal) diameter");
        }
        // Hand.
        th.leftHand = spec ? isLeftHandThread(h->threadSpec) : p_.leftHand;
        const ThreadTravel travel = threadTravel(f.internal, !th.leftHand, p_.climb, in_.op.cutting.clockwise);
        th.ccw = travel.ccw;
        th.up = travel.up;
        // Length.
        Nm length = 0;
        if (p_.length > 0) {
            length = toNm(p_.length);
        } else if (spec && h->threadDepth > 0) {
            length = toNm(h->threadDepth);
        } else {
            length = f.top - f.bottom;
        }
        th.bottom = f.top - length;
        if (th.bottom < f.bottom) {
            return invalid("The thread at " + at + " is " + mmText(length) + " mm long, but the " +
                           (f.internal ? std::string(f.through ? "hole's bore" : "drilled bore") : "boss") + " is " +
                           mmText(f.top - f.bottom) + " mm long: thread less deep" +
                           (f.internal && !f.through ? ", or drill deeper" : ""));
        }
        // Radial depth.
        th.depth = p_.depth > 0 ? toNm(p_.depth) : basicThreadDepthNm(th.pitch, f.internal);
        const Nm dt = toNm(t.diameter);
        const Nm minor = th.majorDiameter - 2 * th.depth;
        if (minor <= 0) {
            return invalid("The thread depth (" + mmText(th.depth) + " mm) leaves no minor diameter of the " +
                           mmText(th.majorDiameter) + " mm thread at " + at);
        }
        const int n = std::max(1, p_.radialPasses);
        Nm last = 0;
        if (f.internal) {
            if (f.diameter >= th.majorDiameter - toNm(kDiameterTolMm)) {
                return invalid("The " + mmText(f.diameter) + " mm hole at X" + mmText(f.x) + " Y" + mmText(f.y) +
                               " is not smaller than the " + mmText(th.majorDiameter) +
                               " mm thread: there is no thread to cut");
            }
            if (dt >= f.diameter - toNm(kDiameterTolMm)) {
                return invalid("The " + mmText(t.diameter) + " mm thread mill does not fit the " + mmText(f.diameter) +
                               " mm hole at X" + mmText(f.x) + " Y" + mmText(f.y));
            }
            last = divHalfEven(th.majorDiameter - dt, 2);
            if (f.diameter + toNm(kDiameterTolMm) < minor) {
                plan_.warnings.push_back("The " + mmText(f.diameter) + " mm hole at X" + mmText(f.x) + " Y" +
                                         mmText(f.y) + " is smaller than the thread's minor diameter (" +
                                         mmText(minor) + " mm): the thread mill cuts more than the thread form");
            }
        } else {
            last = divHalfEven(minor + dt, 2);
            if (th.majorDiameter > f.diameter + toNm(kThreadDiameterTolMm)) {
                plan_.warnings.push_back("The " + mmText(f.diameter) + " mm boss at X" + mmText(f.x) + " Y" +
                                         mmText(f.y) + " is smaller than the thread's " + mmText(th.majorDiameter) +
                                         " mm major diameter: the crests stay flat");
            } else if (th.majorDiameter + toNm(kThreadDiameterTolMm) < f.diameter) {
                plan_.warnings.push_back("The " + mmText(f.diameter) + " mm boss at X" + mmText(f.x) + " Y" +
                                         mmText(f.y) + " is larger than the thread's " + mmText(th.majorDiameter) +
                                         " mm major diameter: the first pass cuts more than the thread form");
            }
        }
        for (int k = 1; k <= n; ++k) {
            // An even number of nanometres, so every pass's radius has the parity of the last one and the leads of all
            // passes inside a hole start and end at one point.
            const Nm inset = 2 * divHalfEven(th.depth * (n - k), 2 * static_cast<std::int64_t>(n));
            th.radii.push_back(f.internal ? last - inset : last + inset);
        }
        if (p_.springPass) {
            th.radii.push_back(last);
        }
        const Nm smallest = *std::min_element(th.radii.begin(), th.radii.end());
        if (smallest < kMinThreadPathRadiusNm) {
            return invalid("The " + mmText(t.diameter) + " mm thread mill is too large for the " +
                           mmText(th.majorDiameter) + " mm thread at " + at + ": its centre would circle only " +
                           mmText(smallest) + " mm from the axis (at least " + mmText(kMinThreadPathRadiusNm) +
                           " mm); choose a smaller thread mill");
        }
        // Above its flutes (libraries.h): a tool with a neck (neckDiameter) has it from the top of the flutes up to
        // neckLength (else up to the shoulder), then up to the shoulder the narrower of the cutting and the shank
        // diameter, and the shank from the larger of the two. Without a neck: up to the shoulder the narrower of the
        // cutting and the shank diameter (the invariant checker reads it so too), then the shank. Where any of them
        // goes into the thread's length it must clear the bore (inside: the drilled hole, the thread's crests) or the
        // boss (outside: the larger of the cylinder and the major diameter) on every pass.
        const Nm deep = f.top - th.bottom;
        const Nm largest = *std::max_element(th.radii.begin(), th.radii.end());
        const Nm outer = std::max(th.majorDiameter, f.diameter);
        const auto clears = [&](Nm width) {
            return f.internal ? 2 * largest + width <= f.diameter : 2 * smallest - width >= outer;
        };
        const Nm ds = t.shankDiameter > 0 ? toNm(t.shankDiameter) : dt;
        const char* const harm = f.internal ? "rub the bore" : "hit the thread";
        if (t.neckDiameter > 0 && t.fluteLength > 0) {
            const Nm flutes = toNm(t.fluteLength);
            const Nm shoulder = std::max(toNm(t.shoulderLength), flutes);
            const Nm neckTop = t.neckLength > 0 ? toNm(t.neckLength) : shoulder;
            const Nm dn = toNm(t.neckDiameter);
            if (flutes < deep && neckTop > flutes && !clears(dn)) {
                return invalid("At " + at + " the thread is " + mmText(deep) + " mm long, but above its " +
                               mmText(flutes) + " mm of flutes the thread mill's " + mmText(dn) + " mm neck would " +
                               harm + ": use a tool with a narrower neck or longer flutes");
            }
            if (neckTop < deep && shoulder > neckTop && !clears(std::min(dt, ds))) {
                return invalid("At " + at + " the thread is " + mmText(deep) + " mm long, but above its neck (" +
                               mmText(neckTop) + " mm from its tip) the thread mill is " + mmText(std::min(dt, ds)) +
                               " mm wide and would " + harm + ": use a tool with a longer neck");
            }
            const Nm shankStart = std::max(shoulder, neckTop);
            if (shankStart < deep && !clears(ds)) {
                return invalid("At " + at + " the thread is " + mmText(deep) + " mm long, but the thread mill's " +
                               mmText(ds) + " mm shank starts " + mmText(shankStart) + " mm above its tip and would " +
                               harm + ": use a tool with a longer reach");
            }
        } else if (t.fluteLength > 0 && toNm(t.fluteLength) < deep && !clears(std::min(dt, ds))) {
            return invalid("At " + at + " the thread is " + mmText(deep) + " mm long, but above its " +
                           mmText(t.fluteLength) + " mm of flutes the thread mill is " + mmText(std::min(dt, ds)) +
                           " mm wide (its shank: the tool gives no neck diameter), which would " +
                           (f.internal ? "rub the bore" : "hit the thread") +
                           ": give the tool its neck diameter in the tool library, or use a tool with longer flutes");
        }
        if (t.neckDiameter <= 0 && t.shoulderLength > 0 && toNm(t.shoulderLength) < deep && !clears(ds)) {
            return invalid("At " + at + " the thread is " + mmText(deep) + " mm long, but the thread mill's " +
                           mmText(ds) + " mm shank starts " + mmText(t.shoulderLength) +
                           " mm above its tip and would " + (f.internal ? "rub the bore" : "hit the thread") +
                           ": use a tool with a longer reach");
        }
        const double stickOut = in_.op.tool.stickOut;
        const Nm reach = f.featureTop - th.bottom;
        if (stickOut > 0 && toNm(stickOut) < reach + toNm(kHolderClearanceMm)) {
            return invalid("At " + at + " the tool goes " + mmText(reach) + " mm deep, but it sticks out only " +
                           mmText(stickOut) + " mm: the holder needs " + mmText(kHolderClearanceMm) +
                           " mm above the top");
        }
        plan_.threads.push_back(std::move(th));
        return {};
    }

    Status levels()
    {
        const Heights& h = in_.op.heights;
        Nm top = std::numeric_limits<Nm>::min();
        for (const PlannedThread& th : plan_.threads) {
            top = std::max(top, th.featureTop);
        }
        plan_.clearance = top + toNm(h.clearance);
        plan_.retract = top + toNm(h.retract);
        plan_.feedPlane = top + toNm(h.feedPlane);
        return {};
    }

    // The overlap's Z change and the step layout of a thread.
    struct Layout {
        std::int64_t steps = 1, revolutions = 1;
        Nm step = 0, overlapZ = 0;
        double overlapDeg = 0;
    };
    Layout layoutOf(const PlannedThread& th) const
    {
        Layout l;
        const Nm length = th.top - th.bottom;
        if (plan_.multiForm) {
            l.step = plan_.teeth * th.pitch;
            l.steps = std::max<std::int64_t>(1, (length + l.step - 1) / l.step);
            l.revolutions = 1;
            // An overlap whose arc would be shorter than kMinOverlapChordNm on the smallest pass is left out.
            const Nm smallest = *std::min_element(th.radii.begin(), th.radii.end());
            const double chord = 2 * static_cast<double>(smallest) * dmath::sin(plan_.overlapDeg * kPi / 360.0);
            l.overlapDeg = chord >= static_cast<double>(kMinOverlapChordNm) ? plan_.overlapDeg : 0;
            l.overlapZ = l.overlapDeg > 0 ? roundHalfEven(static_cast<double>(th.pitch) * l.overlapDeg / 360.0) : 0;
        } else {
            l.revolutions = std::max<std::int64_t>(1, (length + th.pitch - 1) / th.pitch);
        }
        return l;
    }

    Status link()
    {
        const lib::Tool& t = tool();
        const Nm dt = toNm(t.diameter);
        std::vector<link::Pass> passes;
        link::Options o;
        o.clearance = plan_.clearance;
        o.retract = plan_.retract;
        o.feedPlane = plan_.feedPlane;
        o.toolRadius = std::max<Nm>(1, dt / 2);
        o.centreCutting = false;
        o.feeds.plunge = plunge_;
        o.feeds.leadIn = leadIn_;
        o.feeds.leadOut = leadOut_;
        o.keepOrder = true;
        o.entryOrder = {EntryKind::PreDrill};
        link::Stock stock = link::stockOf(in_.geom);
        std::map<Nm, Regions> slices; // external: the part's slice at a boss's bottom, by height
        for (const PlannedThread& th : plan_.threads) {
            RAY_TRY_STATUS(ctx_.cancel.check());
            const Layout l = layoutOf(th);
            const geom2d::Pt centre{th.x, th.y};
            if (th.internal) {
                stock.cleared.push_back(link::Cleared{
                    th.boreBottom,
                    Regions{geom2d::Region{geom2d::Path::circle(centre, th.boreDiameter / 2, true), {}}}});
                o.preDrills.push_back(link::PreDrill{centre, th.boreDiameter, th.boreBottom});
            } else if (slices.count(th.boreBottom) == 0) {
                auto free = freeAround(stock, th.boreBottom);
                if (!free) {
                    return tl::unexpected(free.error());
                }
                stock.cleared.push_back(link::Cleared{th.boreBottom, *free});
                slices.emplace(th.boreBottom, std::move(*free));
            }
            const Nm leadR = th.internal ? 0 : divHalfEven(dt + th.depth, 2);
            for (std::size_t k = 0; k < th.radii.size(); ++k) {
                const Nm r = th.radii[k];
                std::int32_t feed = feed_;
                if (p_.edgeFeed) {
                    const double edge = th.internal ? static_cast<double>(r) + static_cast<double>(dt) / 2
                                                    : static_cast<double>(r) - static_cast<double>(dt) / 2;
                    if (!(edge > 0)) {
                        return invalid("At " + th.where + " the thread mill's cutting edge would cross the axis");
                    }
                    const double f = static_cast<double>(feed_) * static_cast<double>(r) / edge;
                    if (!(f <= static_cast<double>(std::numeric_limits<std::int32_t>::max()))) {
                        return invalid("The feed at the tool centre is out of range");
                    }
                    feed = static_cast<std::int32_t>(std::max<std::int64_t>(1, roundHalfEven(f)));
                }
                link::Lead lead;
                lead.kind = link::LeadKind::Arc;
                lead.sweepDeg = 180;
                lead.radius = th.internal ? std::max<Nm>(1000, r / 2) : leadR;
                lead.minRadius = lead.radius;
                // Inside: the lead lies towards the centre, which is on the left of counter-clockwise travel.
                lead.side = th.internal == th.ccw ? geom2d::Side::Left : geom2d::Side::Right;
                Regions room;
                if (th.internal) {
                    room = Regions{geom2d::Region{geom2d::Path::circle(centre, r, true), {}}};
                } else {
                    geom2d::Region outside = link::unbounded().front();
                    outside.holes.push_back(geom2d::Path::circle(centre, r, false));
                    room = Regions{std::move(outside)};
                }
                const Nm pitch = th.up ? th.pitch : -th.pitch;
                const Nm overlapZ = th.up ? l.overlapZ : -l.overlapZ;
                for (std::int64_t s = 0; s < l.steps; ++s) {
                    const std::int64_t j = th.up ? s : l.steps - 1 - s;
                    const Nm zb = th.bottom + j * l.step;
                    const Nm zs = th.up ? zb : zb + l.revolutions * th.pitch + l.overlapZ;
                    const Nm zEnd = zs + l.revolutions * pitch + overlapZ;
                    if (std::max(zs, zEnd) > plan_.feedPlane) {
                        return invalid("At " + th.where + " the helix rises to Z" + mmText(std::max(zs, zEnd)) +
                                       ", above the feed plane (Z" + mmText(plan_.feedPlane) +
                                       "): raise the feed plane");
                    }
                    link::Pass pass;
                    pass.start = link::Pt3{th.x + r, th.y, zs};
                    pass.moves =
                        helixMoves(th.x, th.y, r, th.ccw, zs, pitch, l.revolutions, l.overlapDeg, overlapZ, feed);
                    pass.leadIn = lead;
                    pass.leadOut = lead;
                    pass.room = room;
                    pass.entryOrder = {EntryKind::PreDrill};
                    passes.push_back(std::move(pass));
                }
            }
        }
        // Inside holes linking decides: consecutive passes in a hole stay down (the link is a move in Z at the hole's
        // centre, where the lead-out ends and the next lead-in starts, inside the drilled hole); a link from hole to
        // hole crosses material and retracts. Outside, every link retracts.
        o.maxStayDown = p_.internal ? geom2d::kMaxCoord : 0;
        auto linked = link::linkPasses(passes, o, stock, ctx_);
        if (!linked) {
            return tl::unexpected(linked.error());
        }
        plan_.linked = std::move(*linked);
        for (const std::string& w : plan_.linked.warnings) {
            plan_.warnings.push_back(w);
        }
        return {};
    }

    // The stock region less the part's material just above z: free around a boss (the part taken as machined).
    Result<Regions> freeAround(const link::Stock& stock, Nm z)
    {
        Regions material;
        for (const kernel::Shape& s : in_.geom.partShapes) {
            auto slice = geom2d::sliceRegions(s, in_.geom.wcsFromModel, z, ctx_);
            if (!slice) {
                return tl::unexpected(slice.error());
            }
            auto u = geom2d::unite(material, *slice, ctx_);
            if (!u) {
                return tl::unexpected(u.error());
            }
            material = std::move(*u);
        }
        return geom2d::subtract(stock.region, material, ctx_);
    }

#undef RAY_TRY_STATUS

    const GenerateInput& in_;
    JobContext& ctx_;
    Params p_;
    ThreadmillPlan plan_;
    std::vector<Feature> features_;
    std::map<std::size_t, std::vector<RecognizedHole>> recognised_;
    std::int32_t feed_ = 0, plunge_ = 0, leadIn_ = 0, leadOut_ = 0;
};

} // namespace

ThreadTravel threadTravel(bool internal, bool rightHand, bool climb, bool spindleClockwise)
{
    ThreadTravel t;
    t.ccw = spindleClockwise;
    if (!climb) {
        t.ccw = !t.ccw;
    }
    if (!internal) {
        t.ccw = !t.ccw;
    }
    t.up = t.ccw == rightHand;
    return t;
}

Nm basicThreadDepthNm(Nm pitch, bool internal)
{
    return geom2d::roundNm(static_cast<double>(pitch) * (internal ? kInternalDepthPerPitch : kExternalDepthPerPitch));
}

std::vector<Move> helixMoves(Nm x, Nm y, Nm radius, bool ccw, Nm z0, Nm pitch, std::int64_t revolutions,
                             double overlapDeg, Nm overlapZ, std::int32_t feedMicronPerMin)
{
    std::vector<Move> out;
    const Nm half = pitch >= 0 ? divHalfEven(pitch, 2) : -divHalfEven(-pitch, 2);
    const auto arc = [&](Nm ex, Nm ey, Nm z) {
        Move m{};
        m.kind = ccw ? MoveKind::ArcCCW : MoveKind::ArcCW;
        m.role = MoveRole::Cut;
        m.plane = ArcPlane::XY;
        m.feedMicronPerMin = feedMicronPerMin;
        m.x = ex;
        m.y = ey;
        m.z = z;
        m.i = x;
        m.j = y;
        m.k = z;
        out.push_back(m);
    };
    for (std::int64_t r = 0; r < revolutions; ++r) {
        const Nm z = z0 + r * pitch;
        arc(x - radius, y, z + half);
        arc(x + radius, y, z + pitch);
    }
    if (overlapDeg > 0) {
        const double a = (ccw ? overlapDeg : -overlapDeg) * (kPi / 180.0);
        const Nm ex = x + geom2d::roundNm(static_cast<double>(radius) * dmath::cos(a));
        const Nm ey = y + geom2d::roundNm(static_cast<double>(radius) * dmath::sin(a));
        arc(ex, ey, z0 + revolutions * pitch + overlapZ);
    }
    return out;
}

Result<ThreadmillPlan> planThreadmill(const GenerateInput& in, JobContext& ctx)
{
    try {
        return Planner(in, ctx).run();
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Out of memory while planning the thread milling operation");
    }
}

} // namespace ray::cam::ops
