// The measure.* actions (task T-022): ray_api/measure.h behind the action registry, for the command palette,
// scripts and agents.
//
// Every action names elements of the document's latest build (the active document unless "document" is given):
//   element = {"body": "<body uuid>" (the first built body when omitted), "kind": "vertex" | "edge" | "face" | "body",
//              "index": <SubId index in the body's shape> (0 when omitted; ignored for "body")}
// They are long-running actions: the shapes are looked up on the UI thread, the kernel queries run on a worker. The
// result is ray::api::measure::toJson (values in mm, mm², mm³, radians, grams and g/cm³, plus witness geometry).
#include "measure_actions.h"

#include "app_impl.h"

#include <ray_api/measure.h>
#include <ray_api/viewer.h>

#include <string>
#include <utility>
#include <vector>

namespace ray::api::detail {
namespace {

using json::Value;

constexpr const char* kUuidPattern = "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$";

Value prop(const char* type, const char* description)
{
    return Value::object({{"type", Value::string(type)}, {"description", Value::string(description)}});
}

std::string schemaOf(Value properties, std::vector<std::string> required = {})
{
    Value req = Value::array();
    for (auto& r : required)
        req.push(Value::string(std::move(r)));
    Value schema = Value::object({{"$schema", Value::string("https://json-schema.org/draft/2020-12/schema")},
                                  {"type", Value::string("object")},
                                  {"additionalProperties", Value::boolean(false)},
                                  {"properties", std::move(properties)}});
    if (!req.asArray().empty())
        schema.set("required", std::move(req));
    return json::canonical(schema);
}

Value documentProp()
{
    return prop("string", "The OpenDocument id; the active document when omitted");
}

Value bodyIdProp(const char* description)
{
    Value v = prop("string", description);
    v.set("pattern", Value::string(kUuidPattern));
    return v;
}

Value elementProp(const char* description)
{
    Value kind = Value::object(
        {{"enum",
          Value::array({Value::string("vertex"), Value::string("edge"), Value::string("face"), Value::string("body")})},
         {"description", Value::string("What the index counts: vertices, edges or faces of the body, or the whole "
                                       "body")}});
    Value index = prop("integer", "SubId index in the body's shape (kernel canonical order); ignored for a body");
    index.set("minimum", Value::integer(0));
    index.set("default", Value::integer(0));
    return Value::object({{"type", Value::string("object")},
                          {"description", Value::string(description)},
                          {"additionalProperties", Value::boolean(false)},
                          {"required", Value::array({Value::string("kind")})},
                          {"properties", Value::object({{"body", bodyIdProp("Body id; the first built body when "
                                                                            "omitted")},
                                                        {"kind", std::move(kind)},
                                                        {"index", std::move(index)}})}});
}

Value densityProp()
{
    Value v = prop("number", "Density in g/cm\u00b3 for the mass (7.85, steel, when omitted)");
    v.set("exclusiveMinimum", Value::integer(0));
    v.set("default", Value::number(measure::kDefaultDensity));
    return v;
}

Value bodiesProp(const char* description)
{
    return Value::object({{"type", Value::string("array")},
                          {"items", bodyIdProp("Body id")},
                          {"description", Value::string(description)}});
}

Result<Value> parseArgs(std::string_view argsJson)
{
    auto parsed = json::parse(argsJson);
    if (!parsed || !parsed->isObject())
        return fail(ErrorCode::InvalidArgument, "The arguments must be a JSON object");
    return std::move(*parsed);
}

// The built bodies of a document, taken on the UI thread (shapes are immutable handles, safe on any thread after).
Result<std::vector<viewer::Body>> bodiesOnUi(AppImpl& app, const std::string& documentId, JobContext& ctx)
{
    return app.bridge().call<std::vector<viewer::Body>>(
        [&app, documentId]() -> Result<std::vector<viewer::Body>> {
            auto entry = app.resolve(documentId);
            if (!entry)
                return tl::unexpected<Error>(entry.error());
            const doc::Document& d = *(*entry)->od.document;
            const doc::BuildResultPtr build = d.build();
            const doc::DocStatePtr state = d.builtState();
            if (!build || !state)
                return fail(ErrorCode::NotFound, "The document has not been built yet; try again when it has");
            return viewer::bodiesFromDocument(*state, *build);
        },
        ctx.cancel);
}

Result<const viewer::Body*> findBody(const std::vector<viewer::Body>& bodies, const std::string& id)
{
    if (bodies.empty())
        return fail(ErrorCode::NotFound, "The document has no built body to measure");
    if (id.empty())
        return &bodies.front();
    const BodyId wanted{Uuid::fromString(id)};
    for (const viewer::Body& b : bodies) {
        if (b.id == wanted)
            return &b;
    }
    return fail(ErrorCode::NotFound, "There is no built body " + id);
}

Result<measure::Element> elementOf(const std::vector<viewer::Body>& bodies, const Value* v, const char* what)
{
    if (v == nullptr || !v->isObject())
        return fail(ErrorCode::InvalidArgument, std::string("The element '") + what + "' is missing");
    auto body = findBody(bodies, v->stringOr("body", ""));
    if (!body)
        return tl::unexpected<Error>(body.error());
    const std::string kind = v->stringOr("kind", "");
    const double index = v->numberOr("index", 0);
    measure::Element e;
    e.shape = (*body)->shape;
    e.name = (*body)->name;
    if (kind == "body") {
        e.sub = kernel::SubId{kernel::SubKind::Solid, 0};
        return e;
    }
    const kernel::SubKind sk = kind == "vertex" ? kernel::SubKind::Vertex
                               : kind == "edge" ? kernel::SubKind::Edge
                                                : kernel::SubKind::Face;
    const std::uint32_t count = e.shape.count(sk);
    if (!(index >= 0) || index >= double(count)) {
        return fail(ErrorCode::InvalidArgument, std::string("/") + what + "/index: the body '" + e.name + "' has " +
                                                    std::to_string(count) + " " + kind + (count == 1 ? "" : "s"));
    }
    e.sub = kernel::SubId{sk, static_cast<std::uint32_t>(index)};
    return e;
}

std::vector<kernel::Shape> shapesOf(const std::vector<viewer::Body>& bodies, const Value& args, Status& status)
{
    std::vector<kernel::Shape> out;
    const Value* list = args.find("bodies");
    if (list == nullptr || !list->isArray()) {
        for (const viewer::Body& b : bodies)
            out.push_back(b.shape);
        return out;
    }
    for (const Value& id : list->asArray()) {
        auto body = findBody(bodies, id.asString());
        if (!body) {
            status = tl::unexpected<Error>(body.error());
            return {};
        }
        out.push_back((*body)->shape);
    }
    return out;
}

ActionResult resultOf(const measure::Measurement& m)
{
    ActionResult r;
    r.resultJson = measure::toJson(m);
    r.warnings = m.notes;
    return r;
}

using Work = std::function<Result<measure::Measurement>(const std::vector<viewer::Body>&, const Value&, JobContext&)>;

// The common frame of every measure action: arguments, the document's bodies, the measurement, its JSON.
ActionFn measureFn(AppImpl& app, Work work)
{
    return [&app, work = std::move(work)](std::string_view argsJson, Caller, JobContext& ctx) -> Result<ActionResult> {
        auto args = parseArgs(argsJson);
        if (!args)
            return tl::unexpected<Error>(args.error());
        auto bodies = bodiesOnUi(app, args->stringOr("document", ""), ctx);
        if (!bodies)
            return tl::unexpected<Error>(bodies.error());
        auto m = work(*bodies, *args, ctx);
        if (!m)
            return tl::unexpected<Error>(m.error());
        return resultOf(*m);
    };
}

ActionInfo info(const char* id, const char* title, const char* description, std::string schema)
{
    ActionInfo a;
    a.id = id;
    a.title = title;
    a.category = "Measure";
    a.description = description;
    a.argsSchemaJson = std::move(schema);
    a.longRunning = true; // kernel queries: on a worker, never on the UI thread
    return a;
}

} // namespace

Status registerMeasureActions(AppImpl& app)
{
    IActionRegistry& actions = app.actions();
    Status st;

    st = actions.add(
        info("measure.distance", "Measure Distance",
             "Minimum distance between two elements with its X, Y and Z components; plane, axis and centre distances "
             "where they apply",
             schemaOf(Value::object({{"a", elementProp("The first element")},
                                     {"b", elementProp("The second element")},
                                     {"document", documentProp()}}),
                      {"a", "b"})),
        measureFn(app,
                  [](const std::vector<viewer::Body>& bodies, const Value& args,
                     JobContext& ctx) -> Result<measure::Measurement> {
                      auto a = elementOf(bodies, args.find("a"), "a");
                      if (!a)
                          return tl::unexpected<Error>(a.error());
                      auto b = elementOf(bodies, args.find("b"), "b");
                      if (!b)
                          return tl::unexpected<Error>(b.error());
                      return measure::distance(*a, *b, ctx);
                  }));
    if (!st)
        return st;

    st = actions.add(
        info("measure.angle", "Measure Angle",
             "Angle between two planar faces, two straight edges or axes, or a line and a plane: the acute angle and "
             "its obtuse supplement",
             schemaOf(Value::object({{"a", elementProp("The first element")},
                                     {"b", elementProp("The second element")},
                                     {"document", documentProp()}}),
                      {"a", "b"})),
        measureFn(app,
                  [](const std::vector<viewer::Body>& bodies, const Value& args,
                     JobContext& ctx) -> Result<measure::Measurement> {
                      auto a = elementOf(bodies, args.find("a"), "a");
                      if (!a)
                          return tl::unexpected<Error>(a.error());
                      auto b = elementOf(bodies, args.find("b"), "b");
                      if (!b)
                          return tl::unexpected<Error>(b.error());
                      return measure::angle(*a, *b, ctx);
                  }));
    if (!st)
        return st;

    st = actions.add(
        info("measure.radius", "Measure Radius",
             "Radius and diameter of a circular edge or a cylindrical face; cone half angle; sphere and torus radii",
             schemaOf(Value::object({{"element", elementProp("A round edge or face")}, {"document", documentProp()}}),
                      {"element"})),
        measureFn(app,
                  [](const std::vector<viewer::Body>& bodies, const Value& args,
                     JobContext& ctx) -> Result<measure::Measurement> {
                      auto e = elementOf(bodies, args.find("element"), "element");
                      if (!e)
                          return tl::unexpected<Error>(e.error());
                      return measure::radius(*e, ctx);
                  }));
    if (!st)
        return st;

    st = actions.add(info("measure.massProperties", "Mass Properties",
                          "Volume, surface area, centroid and mass (at a density) of bodies",
                          schemaOf(Value::object({{"bodies", bodiesProp("Body ids; every built body when omitted")},
                                                  {"density", densityProp()},
                                                  {"document", documentProp()}}))),
                     measureFn(app,
                               [](const std::vector<viewer::Body>& bodies, const Value& args,
                                  JobContext& ctx) -> Result<measure::Measurement> {
                                   Status found = {};
                                   const std::vector<kernel::Shape> shapes = shapesOf(bodies, args, found);
                                   if (!found)
                                       return tl::unexpected<Error>(found.error());
                                   return measure::massProperties(
                                       shapes, args.numberOr("density", measure::kDefaultDensity), ctx);
                               }));
    if (!st)
        return st;

    st = actions.add(info("measure.boundingBox", "Bounding Box",
                          "The axis-aligned bounding box of bodies, with its sizes",
                          schemaOf(Value::object({{"bodies", bodiesProp("Body ids; every built body when omitted")},
                                                  {"document", documentProp()}}))),
                     measureFn(app,
                               [](const std::vector<viewer::Body>& bodies, const Value& args,
                                  JobContext& ctx) -> Result<measure::Measurement> {
                                   Status found = {};
                                   const std::vector<kernel::Shape> shapes = shapesOf(bodies, args, found);
                                   if (!found)
                                       return tl::unexpected<Error>(found.error());
                                   return measure::boundingBox(shapes, ctx);
                               }));
    if (!st)
        return st;

    Value elements = Value::object({{"type", Value::string("array")},
                                    {"minItems", Value::integer(1)},
                                    {"items", elementProp("A selected element")},
                                    {"description", Value::string("The selection, in the order it was made")}});
    st = actions.add(
        info(
            "measure.selection", "Measure Selection",
            "Picks the right measurement: one element's own measures, or the distance and angle between two",
            schemaOf(Value::object(
                         {{"elements", std::move(elements)}, {"density", densityProp()}, {"document", documentProp()}}),
                     {"elements"})),
        measureFn(app,
                  [](const std::vector<viewer::Body>& bodies, const Value& args,
                     JobContext& ctx) -> Result<measure::Measurement> {
                      std::vector<measure::Element> list;
                      const Value* items = args.find("elements");
                      for (std::size_t i = 0; items != nullptr && i < items->asArray().size(); ++i) {
                          const std::string where = "elements/" + std::to_string(i);
                          auto e = elementOf(bodies, &items->asArray()[i], where.c_str());
                          if (!e)
                              return tl::unexpected<Error>(e.error());
                          list.push_back(std::move(*e));
                      }
                      return measure::selection(list, args.numberOr("density", measure::kDefaultDensity), ctx);
                  }));
    return st;
}

} // namespace ray::api::detail
