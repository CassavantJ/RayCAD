// Shared by the parametric feature types (task T-096): reading props with messages a machinist can act on, numeric
// props that are numbers or expressions, the JSON form of naming::TopoRef, plane frames, and the generic
// "feature.<type>.add" / "feature.<type>.edit" commands. Private to src/doc and its tests.
#pragma once

#include "../json.h"
#include "../rebuild.h"

#include <ray/document.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ray::doc::features {

// JSON Schema fragments of the props schemas (the subset src/api/schema.h validates: no $ref). Numeric props carry
// the annotation "x-unit": "length" | "angle" for editors.
std::string uuidSchema();
std::string hashesSchema();
std::string lengthSchema(std::string_view description);
std::string angleSchema(std::string_view description);
std::string topoRefSchema();
std::string planeSchema();
// {"$schema":..., "type":"object", "additionalProperties":false, "required":[required], "properties":{properties}}
std::string objectSchema(std::string_view required, std::string_view properties);

tl::unexpected<Error> invalid(std::string message);
tl::unexpected<Error> failed(ErrorCode code, std::string message);

// Reads one props object. Every error is InvalidArgument "<where>: '<field>' <what>"; `where` is the feature name
// (or the command name). `strict` rejects keys the type does not know (commands); the rebuild ignores them, so a file
// from a newer version still builds.
class PropsReader {
public:
    PropsReader(std::string where, const json::Value& object) : where_(std::move(where)), object_(&object) {}
    // Parses `propsJson` (empty means {}); InvalidArgument if it is not a JSON object.
    static Result<json::Value> parse(std::string_view where, std::string_view propsJson);

    const json::Value& object() const { return *object_; }
    const std::string& where() const { return where_; }
    PropsReader child(const json::Value& object) const { return PropsReader(where_, object); }

    tl::unexpected<Error> error(std::string_view field, const std::string& what) const;
    tl::unexpected<Error> error(const std::string& what) const;

    Status onlyKeys(std::initializer_list<std::string_view> keys, std::string_view prefix = {}) const;
    const json::Value* find(std::string_view key) const { return object_->find(key); }

    Result<Uuid> uuid(std::string_view key, std::string_view field) const;
    Result<std::optional<Uuid>> optUuid(std::string_view key, std::string_view field) const;
    Result<bool> optBool(std::string_view key, std::string_view field, bool fallback) const;
    // One of `choices`; `fallback` when absent (nullopt fallback: required).
    Result<std::string> choice(std::string_view key, std::string_view field,
                               std::initializer_list<std::string_view> choices,
                               std::optional<std::string_view> fallback) const;
    // A number (mm or rad) or an expression string, kept unevaluated. `fallback` when absent (nullopt: required).
    Result<json::Value> numeric(std::string_view key, std::string_view field,
                                std::optional<json::Value> fallback = std::nullopt) const;

private:
    std::string where_;
    const json::Value* object_;
};

// Evaluates a numeric prop: a JSON number is taken as mm (lengths) or rad (angles); a string is an expression
// (document.h, evaluate) over the document parameters. Errors name the feature and the field.
Result<double> evaluateNumeric(const json::Value& value, const std::map<std::string, double>& params,
                               Parameter::Kind kind, std::string_view where, std::string_view field);

// The document parameters evaluated for a command's checks; `error` says why they do not evaluate.
struct DocumentParams {
    std::map<std::string, double> values;
    std::optional<std::string> error;
};
DocumentParams documentParams(const DocState& state);
// evaluateNumeric, adding why the parameters do not evaluate when that is the cause.
Status checkNumeric(const json::Value& value, const DocumentParams& params, Parameter::Kind kind,
                    std::string_view where, std::string_view field);

// ---------- naming::TopoRef as JSON ----------
// The JSON form is naming's (naming.h toJson and topoRefFromJson, the one implementation, also used by the
// model.reference action); these move it in and out of a props JSON value. A reading error names the member under
// `field` ("edges[0].names must list at least one name"). Whether a feature may use a resolution is
// naming::Resolved::usable() (it honours TopoRef::repairConfirmed).
json::Value topoRefToJson(const naming::TopoRef& ref);
Result<naming::TopoRef> topoRefFromJson(const PropsReader& r, const json::Value& value, const std::string& field);
const char* subKindName(kernel::SubKind kind);

// ---------- Frames ----------
// Principal planes: "xy" (normal +Z, x +X), "yz" (normal +X, x +Y), "xz" (normal -Y, x +X; its y is +Z).
std::optional<kernel::Plane> principalPlane(std::string_view name);
Vec3 planePoint(const kernel::Plane& plane, Vec2 uv);
Vec3 planeYDir(const kernel::Plane& plane);
Vec3 vadd(Vec3 a, Vec3 b);
Vec3 vsub(Vec3 a, Vec3 b);
Vec3 vmul(Vec3 a, double s);
double vdot(Vec3 a, Vec3 b);
Vec3 vcross(Vec3 a, Vec3 b);
double vnorm(Vec3 a);
std::optional<Vec3> vunit(Vec3 a);

// The frame of a planar face reference, resolved at this point of the rebuild: outward normal, origin the point of
// the plane nearest the world origin, x the world X projected onto the plane (world Y when X is nearly normal to
// it). Exact, Split (every piece on one plane) and Merged references are used; Repaired, Broken or a face that is not
// planar fail with a message that names `what` (e.g. "the sketch plane of Sketch 2").
Result<kernel::Plane> facePlane(const RebuildContext& ctx, const naming::TopoRef& ref, const std::string& what);

// What a datum plane publishes for later features (a sketch publishes a SketchArtifact, sketch_feature.h).
struct PlaneArtifact : detail::FeatureArtifact {
    kernel::Plane plane;
};

// A plane reference as stored in props: {"principal": "xy" | "yz" | "xz"}, {"datum": "<datum_plane feature id>"} or
// {"face": TopoRef}.
struct PlaneRef {
    std::string principal;
    std::optional<FeatureId> datum;
    std::optional<naming::TopoRef> face;
};
Result<PlaneRef> readPlaneRef(const PropsReader& r, std::string_view key, std::string_view field);
// Resolves a plane reference in a rebuild (a datum through the artifact it published).
Result<kernel::Plane> resolvePlaneRef(const PlaneRef& ref, RebuildContext& ctx, const Feature& self,
                                      const std::string& what);

// The feature `id` of `state`, if it comes before `self` in the history; else an error naming what is wrong with it
// (missing, later in the history, suppressed, of another type) for "<self name>: <role> ...".
Result<const Feature*> earlierFeature(const DocState& state, const Feature& self, const FeatureId& id,
                                      std::string_view type, std::string_view role);

std::string featureLabel(const Feature& feature); // the name, or the type when it has none

// ---------- Commands "feature.<type>.add" and "feature.<type>.edit" ----------
// Implemented by the parametric feature types besides IFeatureType.
class ParametricType {
public:
    virtual ~ParametricType() = default;
    // Checks `feature` (its props strictly) as it would stand at `index` of `state`: props, expressions against the
    // document parameters, and features it names (they must come earlier). Errors are InvalidArgument.
    virtual Status validate(const DocState& state, std::size_t index, const Feature& feature) const = 0;
    // Bodies the feature creates (an extrude or revolve with operation "new"): the add command lists them in the
    // document's bodies.
    virtual std::vector<BodyId> createdBodies(const Feature&) const { return {}; }
    // The props the add and edit commands store for `feature`, checked by validate() as it stands in `state`: by
    // default its props unchanged. A sketch stores its coordinates solved (sketch_feature.h, storedSketch).
    virtual Result<std::string> storedProps(const DocState& state, const Feature& feature) const
    {
        (void)state;
        return feature.propsJson;
    }
    // The user-facing title of the type: "Extrude", "Sketch", ...
    virtual std::string_view title() const = 0;
};

// The two commands of one parametric type (`type` must be registered in builtinFeatureRegistry()).
//   feature.<type>.add   {"id", "name"?, "props", "index"?, "suppressed"?, "bodyName"?}
//   feature.<type>.edit  {"id", "name"?, "props"?}   "props" replaces the given top-level keys (null removes one)
std::vector<std::unique_ptr<ICommandHandler>> makeTypedFeatureCommands(std::string_view type,
                                                                       std::string_view propsSchema);

} // namespace ray::doc::features
