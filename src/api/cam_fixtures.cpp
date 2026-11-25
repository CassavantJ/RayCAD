// A library vise placed on a setup's stock (ray_api/cam_fixtures.h). The move arithmetic is the transform_body
// feature's own (src/doc/features/transform_body_feature.h, private to doc and its friends, as feature_edit.cpp uses
// doc's), so what is planned here is what the rebuild does.
#include <ray_api/cam_fixtures.h>

#include <ray_api/feature_edit.h>

#include "../doc/features/transform_body_feature.h"
#include "json.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <exception>
#include <new>
#include <set>
#include <utility>

namespace ray::api::cam_fixtures {
namespace {

using json::Value;
namespace fx = ray::doc::features;

constexpr double kPi = 3.14159265358979323846;
constexpr double kTransformTol = 1e-6; // mm: where a library model's face must be (a STEP round trip keeps 1e-7)

template <class T>
Result<T> invalid(std::string message)
{
    return tl::unexpected(Error{ErrorCode::InvalidArgument, std::move(message), {}});
}

// ---------- 3x4 rigid moves ----------
Vec3 applyPoint(const Xform& x, Vec3 p)
{
    const double* m = x.m;
    return Vec3{m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}

Vec3 applyVector(const Xform& x, Vec3 v)
{
    const double* m = x.m;
    return Vec3{m[0] * v.x + m[1] * v.y + m[2] * v.z, m[4] * v.x + m[5] * v.y + m[6] * v.z,
                m[8] * v.x + m[9] * v.y + m[10] * v.z};
}

// a then b's frame: compose(a, b) p = a (b p).
Xform compose(const Xform& a, const Xform& b)
{
    Xform out{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            double v = a.m[i * 4 + 0] * b.m[0 + j] + a.m[i * 4 + 1] * b.m[4 + j] + a.m[i * 4 + 2] * b.m[8 + j];
            if (j == 3) {
                v += a.m[i * 4 + 3];
            }
            out.m[i * 4 + j] = v;
        }
    }
    return out;
}

Xform translation(Vec3 d)
{
    Xform x = Xform::identity();
    x.m[3] = d.x;
    x.m[7] = d.y;
    x.m[11] = d.z;
    return x;
}

Vec3 sub(Vec3 a, Vec3 b)
{
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

double dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 cross(Vec3 a, Vec3 b)
{
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

double length(Vec3 a)
{
    return std::sqrt(dot(a, a));
}

double nm(double mm)
{
    return std::nearbyint(mm * 1e6) / 1e6;
}

// The largest difference between two moves' entries (rotation) and their translations.
double rotationGap(const Xform& a, const Xform& b)
{
    double worst = 0;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            worst = std::max(worst, std::fabs(a.m[i * 4 + j] - b.m[i * 4 + j]));
        }
    }
    return worst;
}

// ---------- Text ----------
std::string fixed(double v, int decimals)
{
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, decimals);
    std::string s(buf, r.ptr);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') {
            s.pop_back();
        }
        if (!s.empty() && s.back() == '.') {
            s.pop_back();
        }
    }
    if (s == "-0") {
        s = "0";
    }
    return s;
}

// ---------- The document ----------
const doc::Body* bodyIn(const doc::DocState& state, const BodyId& id)
{
    const auto it =
        std::find_if(state.bodies.begin(), state.bodies.end(), [&](const doc::Body& b) { return b.id == id; });
    return it == state.bodies.end() ? nullptr : &*it;
}

std::optional<std::size_t> featureIndex(const doc::DocState& state, const FeatureId& id)
{
    for (std::size_t i = 0; i < state.features.size(); ++i) {
        if (state.features[i].id == id) {
            return i;
        }
    }
    return std::nullopt;
}

Value propsOf(const doc::Feature& f)
{
    auto v = json::parse(f.propsJson);
    return v && v->isObject() ? std::move(*v) : Value::object();
}

// The body a transform_body feature's mate target is on ("" without a mate).
std::string mateTargetBody(const doc::Feature& f)
{
    const Value props = propsOf(f);
    const Value* mate = props.find("mate");
    const Value* target = mate != nullptr ? mate->find("target") : nullptr;
    return target != nullptr ? target->stringOr("body", "") : std::string();
}

// A face of a library model as the transform_body mate reads it, and its element.
struct LibraryFace {
    kernel::SubId sub;
    Vec3 centroid, normal;
};

// The planar face of `shape` whose outward normal is `normal`, whose centroid is `centroid` in the coordinates `check`
// says (x, y, z each when asked) and whose area is `area`.
std::optional<LibraryFace> findFace(const kernel::Shape& shape, Vec3 normal, std::optional<double> x,
                                    std::optional<double> y, std::optional<double> z, double area)
{
    for (std::uint32_t i = 0; i < shape.count(kernel::SubKind::Face); ++i) {
        const kernel::SubId s{kernel::SubKind::Face, i};
        auto info = kernel::surfaceInfo(shape, s);
        if (!info || info->type != kernel::SurfaceType::Plane) {
            continue;
        }
        auto sample = kernel::faceSample(shape, s);
        if (!sample || length(sub(sample->normal, normal)) > 1e-7) {
            continue;
        }
        const Vec3 c = info->centroid;
        if ((x && std::fabs(c.x - *x) > kTransformTol) || (y && std::fabs(c.y - *y) > kTransformTol) ||
            (z && std::fabs(c.z - *z) > kTransformTol) || std::fabs(info->area - area) > 1e-6 * area) {
            continue;
        }
        return LibraryFace{s, c, normal};
    }
    return std::nullopt;
}

Result<Value> refValue(const doc::BuiltBody& built, const BodyId& body, kernel::SubId sub)
{
    auto resolver = naming::makeResolver({naming::ResolverBody{body, built.map, built.shape}});
    if (!resolver) {
        return tl::unexpected(Error{ErrorCode::Internal, "Out of memory", {}});
    }
    const naming::TopoRef ref = resolver->makeRef(body, sub);
    if (ref.names.empty()) {
        return tl::unexpected(Error{ErrorCode::Internal, "A face of the vise has no persistent name", {}});
    }
    return json::parse(naming::toJson(ref));
}

// The transform_body move the document builds from these texts (no mate).
Result<Xform> evaluatedFree(const doc::DocState& state, const FreeMove& m, const std::string& unit)
{
    fx::Placement p;
    const struct {
        double value;
        bool angle;
        double* out;
    } fields[] = {{m.tiltX, true, &p.tiltX},        {m.tiltY, true, &p.tiltY},        {m.angle, true, &p.angle},
                  {m.offset.x, false, &p.offset.x}, {m.offset.y, false, &p.offset.y}, {m.offset.z, false, &p.offset.z}};
    for (const auto& f : fields) {
        auto v = feature_edit::evaluate(state, f.angle ? angleText(f.value) : lengthText(f.value, unit), f.angle);
        if (!v) {
            return tl::unexpected(v.error());
        }
        *f.out = *v;
    }
    return fx::placementTransform(p);
}

// The props of a transform_body without a mate: the texts that are not 0.
Value freeProps(const BodyId& body, const FreeMove& m, const std::string& unit)
{
    Value props = Value::object({{"body", Value::string(body.value.toString())}});
    const struct {
        const char* key;
        double value;
        bool angle;
    } fields[] = {{"tiltX", m.tiltX, true}, {"tiltY", m.tiltY, true}, {"angle", m.angle, true},
                  {"x", m.offset.x, false}, {"y", m.offset.y, false}, {"z", m.offset.z, false}};
    for (const auto& f : fields) {
        const std::string text = f.angle ? angleText(f.value) : lengthText(f.value, unit);
        if (text != "0 deg" && text != "0 mm" && text != "0 in") {
            props.set(f.key, Value::string(text));
        }
    }
    return props;
}

} // namespace

// ---------- Descriptions ----------
Result<FixtureModel> parseFixtureModel(std::string_view text)
{
    auto v = json::parse(text);
    if (!v || !v->isObject()) {
        return invalid<FixtureModel>("A fixture description must be one JSON object");
    }
    if (v->numberOr("schema", 0) != 1) {
        return invalid<FixtureModel>("A fixture description needs \"schema\": 1");
    }
    const auto positive = [&](const char* key, double& out) -> Status {
        const double n = v->numberOr(key, 0);
        if (!(n > 0) || !std::isfinite(n)) {
            return tl::unexpected(
                Error{ErrorCode::InvalidArgument,
                      std::string("A fixture description needs \"") + key + "\" as a positive number of millimetres",
                      {}});
        }
        out = n;
        return {};
    };
    FixtureModel out;
    const std::string kind = v->stringOr("kind", "");
    if (kind == "vise") {
        out.kind = FixtureModel::Kind::Vise;
        out.vise.body = v->stringOr("body", "");
        out.vise.movingJaw = v->stringOr("movingJaw", "");
        if (out.vise.body.empty() || out.vise.movingJaw.empty()) {
            return invalid<FixtureModel>("A vise description names its \"body\" and its \"movingJaw\"");
        }
        for (const auto& [key, slot] : {std::pair<const char*, double*>{"jawLength", &out.vise.jawLength},
                                        std::pair<const char*, double*>{"jawHeight", &out.vise.jawHeight},
                                        std::pair<const char*, double*>{"maxOpening", &out.vise.maxOpening}}) {
            if (auto ok = positive(key, *slot); !ok) {
                return tl::unexpected(ok.error());
            }
        }
        return out;
    }
    if (kind == "parallels") {
        out.kind = FixtureModel::Kind::Parallels;
        const Value* bodies = v->find("bodies");
        if (bodies == nullptr || !bodies->isArray() || bodies->asArray().size() != 2) {
            return invalid<FixtureModel>("A parallels description names its two \"bodies\"");
        }
        for (const Value& b : bodies->asArray()) {
            if (!b.isString() || b.asString().empty()) {
                return invalid<FixtureModel>("A parallels description names its two \"bodies\"");
            }
            out.parallels.bodies.push_back(b.asString());
        }
        for (const auto& [key, slot] : {std::pair<const char*, double*>{"height", &out.parallels.height},
                                        std::pair<const char*, double*>{"thickness", &out.parallels.thickness}}) {
            if (auto ok = positive(key, *slot); !ok) {
                return tl::unexpected(ok.error());
            }
        }
        return out;
    }
    return invalid<FixtureModel>("A fixture description's \"kind\" is \"vise\" or \"parallels\", not \"" + kind + "\"");
}

// ---------- The fit ----------
ViseFit fitVise(const Box3& s, Grip grip, const ViseModel& vise, const ParallelsModel* parallels)
{
    ViseFit f;
    const double dx = s.max.x - s.min.x;
    const double dy = s.max.y - s.min.y;
    f.acrossX = grip == Grip::AcrossX || (grip == Grip::Shorter && dx < dy);
    f.across = f.acrossX ? dx : dy;
    f.along = f.acrossX ? dy : dx;
    f.tooWide = f.across > vise.maxOpening;
    f.opening = std::clamp(f.across, 0.0, vise.maxOpening);
    f.floorZ = s.min.z - (parallels != nullptr ? parallels->height : 0.0);
    const double jawTop = f.floorZ + vise.jawHeight;
    f.height = s.max.z - s.min.z;
    f.jawAboveStock = jawTop - s.max.z;
    f.gripDepth = std::min(jawTop, s.max.z) - s.min.z;
    // Columns: where the vise's X, Y and Z go; the translation: where its origin (the fixed jaw's face, centred along
    // the jaws, on the floor) goes.
    if (f.acrossX) {
        // The fixed jaw against -X, opening towards +X: vise Y = WCS +X, vise X = WCS -Y.
        f.wcsFromVise = Xform{{0, 1, 0, s.min.x, -1, 0, 0, (s.min.y + s.max.y) / 2, 0, 0, 1, f.floorZ}};
    } else {
        // The fixed jaw against +Y (the back), opening towards -Y: vise Y = WCS -Y, vise X = WCS -X.
        f.wcsFromVise = Xform{{-1, 0, 0, (s.min.x + s.max.x) / 2, 0, -1, 0, s.max.y, 0, 0, 1, f.floorZ}};
    }
    return f;
}

std::optional<ParallelsSuggestion> tallerParallels(const ViseFit& fit, const ViseModel& vise,
                                                   const ParallelsModel* current,
                                                   const std::vector<ParallelsModel>& pairs)
{
    if (!(fit.jawAboveStock > kLevelTolMm)) {
        return std::nullopt;
    }
    const double now = current != nullptr ? current->height : 0.0;
    std::optional<ParallelsSuggestion> above, nearest;
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        const ParallelsModel& p = pairs[i];
        if (!(p.height > now + kLevelTolMm) || fit.opening + kLevelTolMm < 2 * p.thickness) {
            continue;
        }
        // The stock's bottom on the pair's top: the jaws' top jawHeight above the floor, the pair's height below it.
        ParallelsSuggestion s;
        s.index = i;
        s.jawAboveStock = vise.jawHeight - p.height - fit.height;
        s.gripDepth = std::min(vise.jawHeight - p.height, fit.height);
        s.aboveJaws = s.jawAboveStock <= kLevelTolMm;
        if (s.gripDepth + kLevelTolMm < kMinGripMm) {
            continue;
        }
        if (s.aboveJaws) {
            if (!above || p.height < pairs[above->index].height) {
                above = s;
            }
        } else if (!nearest || p.height > pairs[nearest->index].height) {
            nearest = s;
        }
    }
    return above ? above : nearest;
}

FreeMove freeMove(const Xform& x)
{
    const double* m = x.m;
    const double r00 = m[0], r01 = m[1], r10 = m[4], r11 = m[5], r20 = m[8], r21 = m[9], r22 = m[10];
    FreeMove out;
    const double cb = std::hypot(r00, r10);
    out.tiltY = std::atan2(-r20, cb);
    if (cb > 1e-12) {
        out.angle = std::atan2(r10, r00);
        out.tiltX = std::atan2(r21, r22);
    } else {
        // X goes onto model Z: the turns about X and Z are one; all of it in the angle.
        out.tiltX = 0;
        out.angle = std::atan2(-r01, r11);
    }
    for (double* a : {&out.tiltX, &out.tiltY, &out.angle}) {
        double deg = *a * 180.0 / kPi;
        const double whole = std::nearbyint(deg);
        if (std::fabs(deg - whole) < 1e-9) {
            deg = whole;
        }
        if (deg <= -180.0) {
            deg += 360.0;
        }
        *a = deg == 0.0 ? 0.0 : deg * kPi / 180.0;
    }
    out.offset = Vec3{m[3], m[7], m[11]};
    return out;
}

std::string lengthText(double mm, std::string_view unit)
{
    if (unit == "in") {
        return fixed(mm / 25.4, 8) + " in";
    }
    return fixed(mm, 6) + " mm";
}

std::string angleText(double radians)
{
    return fixed(radians * 180.0 / kPi, 9) + " deg";
}

std::vector<FeatureId> placementsOf(const doc::DocState& state, const BodyId& body)
{
    std::vector<FeatureId> out;
    const std::string id = body.value.toString();
    for (const doc::Feature& f : state.features) {
        if (f.type == fx::kTransformBodyType && propsOf(f).stringOr("body", "") == id) {
            out.push_back(f.id);
        }
    }
    return out;
}

// ---------- The features ----------
Result<PlacePlan> planViseOnStock(const PlaceRequest& rq)
{
    try {
        if (!rq.state || !rq.build) {
            return invalid<PlacePlan>("The model is not built yet");
        }
        const doc::DocState& state = *rq.state;
        const cam::Setup* setup = nullptr;
        if (state.cam) {
            for (const cam::Setup& s : state.cam->setups) {
                setup = s.id == rq.setup ? &s : setup;
            }
        }
        if (setup == nullptr) {
            return tl::unexpected(
                Error{ErrorCode::NotFound, "The document has no setup " + rq.setup.value.toString(), {}});
        }
        const bool withParallels = rq.parallelsModel.has_value();
        if (withParallels && rq.parallels.size() != 2) {
            return invalid<PlacePlan>("Parallels are placed in pairs");
        }

        // Each body as its library file has it: before its placement, or as built when it has none.
        struct Item {
            BodyId id;
            std::string name;
            std::optional<FeatureId> placement;
            doc::BuildResultPtr libraryBuild;
            const doc::BuiltBody* library = nullptr;
        };
        std::vector<BodyId> ids{rq.vise, rq.jaw};
        ids.insert(ids.end(), rq.parallels.begin(), rq.parallels.end());
        std::vector<Item> items;
        for (const BodyId& id : ids) {
            const doc::Body* body = bodyIn(state, id);
            if (body == nullptr) {
                return invalid<PlacePlan>("The document has no body " + id.value.toString());
            }
            Item item{id, body->name, std::nullopt, rq.build, nullptr};
            const std::vector<FeatureId> moves = placementsOf(state, id);
            if (moves.size() > 1) {
                return invalid<PlacePlan>(body->name + " is moved by " + std::to_string(moves.size()) +
                                          " Move Body features; keep one, then place it on the stock again");
            }
            if (!moves.empty()) {
                item.placement = moves.front();
                if (!rq.buildBefore) {
                    return invalid<PlacePlan>("The model before a placement cannot be built here");
                }
                auto before = rq.buildBefore(moves.front());
                if (!before) {
                    return tl::unexpected(before.error());
                }
                item.libraryBuild = *before;
            }
            const auto built = item.libraryBuild ? item.libraryBuild->bodies.find(id)
                                                 : std::map<BodyId, doc::BuiltBody>::const_iterator{};
            if (!item.libraryBuild || built == item.libraryBuild->bodies.end() || built->second.shape.isNull()) {
                return invalid<PlacePlan>(body->name + " is not built");
            }
            item.library = &built->second;
            items.push_back(std::move(item));
        }
        const Item& vise = items[0];
        const Item& jaw = items[1];
        const ViseModel& vm = rq.viseModel;

        // The clamping faces, where the library model has them: the fixed jaw's on y = 0 looking +Y, the moving jaw's
        // looking -Y, both centred on x = 0 and half the jaw's height.
        const double faceArea = vm.jawLength * vm.jawHeight;
        const auto fixedFace = findFace(vise.library->shape, Vec3{0, 1, 0}, 0.0, 0.0, vm.jawHeight / 2, faceArea);
        if (!fixedFace) {
            return invalid<PlacePlan>(
                vise.name + " is not where its library model has it before it is placed (its fixed jaw's face is not "
                            "on y = 0, looking +Y): place it by hand");
        }
        const auto jawFace =
            findFace(jaw.library->shape, Vec3{0, -1, 0}, 0.0, std::nullopt, vm.jawHeight / 2, faceArea);
        if (!jawFace) {
            return invalid<PlacePlan>(jaw.name + " is not where its library model has it before it is placed (its "
                                                 "clamping face, looking -Y, is not found): place it by hand");
        }

        // The fit, and the vise's move as the document will build it from the texts.
        const ViseFit fit = fitVise(rq.stock, rq.grip, vm,
                                    withParallels ? &*rq.parallelsModel : static_cast<const ParallelsModel*>(nullptr));
        const Xform modelFromVise = compose(rq.modelFromWcs, fit.wcsFromVise);
        const FreeMove viseMove = freeMove(modelFromVise);
        auto viseBuilt = evaluatedFree(state, viseMove, rq.unit);
        if (!viseBuilt) {
            return tl::unexpected(viseBuilt.error());
        }

        // The parallels: one against each jaw, standing on the floor, centred along the jaws.
        std::vector<FreeMove> parallelMoves;
        if (withParallels) {
            std::vector<std::pair<Box3, std::size_t>> boxes;
            for (std::size_t i = 2; i < items.size(); ++i) {
                boxes.emplace_back(kernel::bounds(items[i].library->shape), i);
            }
            std::sort(boxes.begin(), boxes.end(), [](const auto& a, const auto& b) {
                return a.first.min.y + a.first.max.y < b.first.min.y + b.first.max.y;
            });
            const double widths =
                (boxes[0].first.max.y - boxes[0].first.min.y) + (boxes[1].first.max.y - boxes[1].first.min.y);
            if (fit.opening + kTransformTol < widths) {
                return invalid<PlacePlan>("The stock is narrower across the jaws than the two parallels side by side: "
                                          "take the parallels out of the setup, or place them by hand");
            }
            parallelMoves.resize(2);
            for (std::size_t k = 0; k < 2; ++k) {
                const Box3& b = boxes[k].first;
                const Vec3 d{nm(-(b.min.x + b.max.x) / 2), nm(k == 0 ? -b.min.y : fit.opening - b.max.y), nm(-b.min.z)};
                parallelMoves[boxes[k].second - 2] = freeMove(compose(modelFromVise, translation(d)));
            }
        }

        // The moving jaw: its clamping face mated to the fixed jaw's where the vise now is, the opening apart, turned
        // so that it is the same way up as the vise.
        fx::Placement jawPlace;
        jawPlace.face = fx::MateFace{fx::roundedPoint(jawFace->centroid), fx::roundedNormal(jawFace->normal)};
        jawPlace.target = fx::MateFace{fx::roundedPoint(applyPoint(*viseBuilt, fixedFace->centroid)),
                                       fx::roundedNormal(applyVector(*viseBuilt, fixedFace->normal))};
        const std::string gapText = lengthText(fit.opening, rq.unit);
        auto gap = feature_edit::evaluate(state, gapText, false);
        if (!gap) {
            return tl::unexpected(gap.error());
        }
        jawPlace.gap = *gap;
        const Xform mated = fx::placementTransform(jawPlace);
        // The turn about the target's normal from the mate's orientation to the vise's.
        const Vec3 n = jawPlace.target->normal;
        Vec3 u = cross(n, Vec3{0, 0, 1});
        if (length(u) < 0.5) {
            u = cross(n, Vec3{1, 0, 0});
        }
        u = Vec3{u.x / length(u), u.y / length(u), u.z / length(u)};
        // R_vise R_mated^T u: u taken back to the jaw's own frame by the mate, then out by the vise's turn.
        const Vec3 back{mated.m[0] * u.x + mated.m[4] * u.y + mated.m[8] * u.z,
                        mated.m[1] * u.x + mated.m[5] * u.y + mated.m[9] * u.z,
                        mated.m[2] * u.x + mated.m[6] * u.y + mated.m[10] * u.z};
        const Vec3 v = applyVector(*viseBuilt, back);
        const double jawTurn = std::atan2(dot(n, cross(u, v)), dot(u, v));
        const std::string jawAngleText = angleText(jawTurn);
        auto jawAngle = feature_edit::evaluate(state, jawAngleText, true);
        if (!jawAngle) {
            return tl::unexpected(jawAngle.error());
        }
        jawPlace.angle = *jawAngle;
        // The check of the arithmetic above: the jaw ends the vise's way up, its face the opening from the fixed one.
        {
            const Xform jawBuilt = fx::placementTransform(jawPlace);
            const Vec3 got = applyPoint(jawBuilt, jawFace->centroid);
            const Vec3 want = applyPoint(*viseBuilt, Vec3{0, fit.opening, vm.jawHeight / 2});
            if (rotationGap(jawBuilt, *viseBuilt) > 1e-9 || length(sub(got, want)) > 1e-5) {
                return tl::unexpected(
                    Error{ErrorCode::Internal, "The moving jaw's mate does not close the vise as planned", {}});
            }
        }
        auto jawFaceRef = refValue(*jaw.library, jaw.id, jawFace->sub);
        if (!jawFaceRef) {
            return tl::unexpected(jawFaceRef.error());
        }
        auto fixedFaceRef = refValue(*vise.library, vise.id, fixedFace->sub);
        if (!fixedFaceRef) {
            return tl::unexpected(fixedFaceRef.error());
        }

        // Where the vise's new placement goes: before what follows it (a mate to it, its jaw's or parallels' moves).
        const auto indexOf = [&](const Item& item) {
            return item.placement ? featureIndex(state, *item.placement) : std::nullopt;
        };
        std::optional<std::size_t> viseIndex;
        if (!vise.placement) {
            for (std::size_t i = 0; i < state.features.size(); ++i) {
                if (state.features[i].type == fx::kTransformBodyType &&
                    mateTargetBody(state.features[i]) == vise.id.value.toString()) {
                    viseIndex = i;
                    break;
                }
            }
            for (std::size_t k = 1; k < items.size(); ++k) {
                if (const auto at = indexOf(items[k]); at && (!viseIndex || *at < *viseIndex)) {
                    viseIndex = at;
                }
            }
        } else if (const auto jawAt = indexOf(jaw); jawAt && *jawAt < *indexOf(vise)) {
            return invalid<PlacePlan>(jaw.name + " is placed before " + vise.name +
                                      " in the history, so it cannot follow the vise: move its Move Body feature "
                                      "after the vise's, then place the vise on the stock again");
        }

        // The commands.
        const auto featureName = [&](const std::string& body) {
            std::string name = rq.namePattern;
            const auto at = name.find("%1");
            return at == std::string::npos ? name : name.replace(at, 2, body);
        };
        const auto newId = [&] {
            return rq.newFeatureId ? rq.newFeatureId() : FeatureId{Uuid::generate()};
        };
        Value commands = Value::array();
        const auto place = [&](const Item& item, Value props, std::optional<std::size_t> index) {
            Value args = Value::object();
            if (item.placement) {
                for (const char* key : {"mate", "gap", "angle", "tiltX", "tiltY", "x", "y", "z"}) {
                    if (props.find(key) == nullptr) {
                        props.set(key, Value());
                    }
                }
                args.set("id", Value::string(item.placement->value.toString()));
                args.set("props", std::move(props));
                commands.push(
                    Value::object({{"name", Value::string("feature.transform_body.edit")}, {"args", std::move(args)}}));
                return;
            }
            args.set("id", Value::string(newId().value.toString()));
            args.set("name", Value::string(featureName(item.name)));
            args.set("props", std::move(props));
            if (index) {
                args.set("index", Value::integer(static_cast<std::int64_t>(*index)));
            }
            commands.push(
                Value::object({{"name", Value::string("feature.transform_body.add")}, {"args", std::move(args)}}));
        };
        place(vise, freeProps(vise.id, viseMove, rq.unit), viseIndex);
        Value jawProps = Value::object(
            {{"body", Value::string(jaw.id.value.toString())},
             {"mate", Value::object({{"face", std::move(*jawFaceRef)}, {"target", std::move(*fixedFaceRef)}})}});
        if (gapText != "0 mm" && gapText != "0 in") {
            jawProps.set("gap", Value::string(gapText));
        }
        if (jawAngleText != "0 deg") {
            jawProps.set("angle", Value::string(jawAngleText));
        }
        // A new jaw placement goes where new features go (after the vise's, wherever that went).
        place(jaw, std::move(jawProps), std::nullopt);
        for (std::size_t k = 0; k < parallelMoves.size(); ++k) {
            place(items[2 + k], freeProps(items[2 + k].id, parallelMoves[k], rq.unit), std::nullopt);
        }

        // Bodies placed for the setup that are not its fixtures yet become fixtures of it.
        PlacePlan plan;
        plan.fit = fit;
        std::vector<BodyId> fixtures = setup->fixtureBodies;
        for (const BodyId& id : ids) {
            if (std::find(fixtures.begin(), fixtures.end(), id) == fixtures.end()) {
                fixtures.push_back(id);
                plan.addedToSetup.push_back(id);
            }
        }
        if (!plan.addedToSetup.empty()) {
            Value list = Value::array();
            for (const BodyId& id : fixtures) {
                list.push(Value::string(id.value.toString()));
            }
            commands.push(Value::object({{"name", Value::string("cam.setup.edit")},
                                         {"args", Value::object({{"id", Value::string(setup->id.value.toString())},
                                                                 {"fixtureBodies", std::move(list)}})}}));
        }
        plan.batchArgsJson = json::write(Value::object({{"commands", std::move(commands)}}));
        return plan;
    } catch (const std::bad_alloc&) {
        return tl::unexpected(Error{ErrorCode::Internal, "Out of memory", {}});
    }
}

} // namespace ray::api::cam_fixtures
