// Operation templates as actions (task T-057): applying templates to the holes of a setup, previewing that, re-applying
// a template to an operation it made, saving an operation as a template, renaming and removing templates; and the
// cam.batch command that applies several CAM commands as one undo step.
//
// Arguments shared by preview and apply (cam.template.preview / cam.template.apply):
//   {"document"?, "setup": uuid,
//    "templates"?: [uuid, ...]     the templates, by id (any library of the stack), or
//    "set"?: "Tapped holes"        the templates of that set for the setup's stock material and machine
//                                  (LibraryStack::templatesFor), most specific first;
//    "holes"?: {"group"?: key, "kind"?: "hole.tapped", "threadSpec"?, "diameter"? (mm, within 0.001), "through"?}
//                                  the recognised holes of the setup that match every given member, or
//    "geometry"?: [TopoRef, ...]   the holes with these faces (any face of each);
//                                  neither: every recognised hole of the setup (each template keeps to its kind)}
// The holes are those cam::recognizeHoles finds on the setup's part bodies along its Z, with the hole features' threads
// (what the drilling operation and the operation panel's hole picker see). The plan is cam::templates::planTemplates
// with the libraries' tools (LibraryStack::queryTools), the document's expression evaluator over its parameters, the
// setup's stock material (else the material the templates name), its machine and the operation panel's default
// heights in the document's units.
// Preview result: {"setup", "holes": n, "groups": [{key, kind, threadSpec, diameter, depth, through,
//   counterboreDiameter, counterboreDepth, countersinkDiameter, count, label, targeted}], "targets": n,
//   "templates": [{id, name, set, sequence}], "operations": [{name, type, template, templateName, tool, toolName,
//   toolNumber, holes, params, rpm, feedCut}], "missing": [{template, templateName, rule, holes}], "untouched": n,
//   "ok": bool, "message": "..." (why it cannot be applied)}. Apply: the same with each operation's "id"; it refuses
// (NotFound, "Nothing was created: " and the message naming each missing tool and its holes) when a tool is missing,
// and when no operation would be made.
// Apply is one cam.batch command (one undo step): cam.library.embed for the library tools (and material) the document
// does not have yet, then cam.operation.add for each operation, at the end of the setup, in the plan's order. Tool
// numbers: the number the document already gives the tool, else its default number when no other tool has it, else
// the next free number.
#include "cam_template_actions.h"

#include "app_impl.h"

#include "../cam/templates.h"

#include <ray_api/cam_tools.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <map>
#include <new>
#include <set>
#include <utility>

namespace ray::api::detail {

namespace {

namespace tpl = cam::templates;
using json::Value;

constexpr const char* kUuidPattern = "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$";

Value prop(const char* type, const char* description)
{
    return Value::object({{"type", Value::string(type)}, {"description", Value::string(description)}});
}

Value uuidProp(const char* description)
{
    Value v = prop("string", description);
    v.set("pattern", Value::string(kUuidPattern));
    return v;
}

std::string schemaOf(Value properties, std::vector<std::string> required = {})
{
    Value req = Value::array();
    for (auto& r : required) {
        req.push(Value::string(std::move(r)));
    }
    Value schema = Value::object({{"$schema", Value::string("https://json-schema.org/draft/2020-12/schema")},
                                  {"type", Value::string("object")},
                                  {"additionalProperties", Value::boolean(false)},
                                  {"properties", std::move(properties)}});
    if (!req.asArray().empty()) {
        schema.set("required", std::move(req));
    }
    return json::canonical(schema);
}

Result<Value> parseArgs(std::string_view argsJson)
{
    auto parsed = json::parse(argsJson);
    if (!parsed || !parsed->isObject()) {
        return fail(ErrorCode::InvalidArgument, "The arguments must be a JSON object");
    }
    return std::move(*parsed);
}

ActionResult result(const Value& v)
{
    ActionResult r;
    r.resultJson = json::canonical(v);
    return r;
}

Result<Value> parseValue(std::string_view text)
{
    auto v = json::parse(text);
    if (!v) {
        return fail(ErrorCode::Internal, "Unreadable JSON: " + v.error().message);
    }
    return std::move(*v);
}

std::string idText(const Uuid& id)
{
    return id.toString();
}

// A content hash as Hash256::hex() writes it; an all-zero hash for anything else (which matches no item).
Hash256 hashFromHex(std::string_view text)
{
    Hash256 h;
    const auto nibble = [](char c) -> int {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    if (text.size() != 64) {
        return h;
    }
    for (std::size_t i = 0; i < 32; ++i) {
        const int hi = nibble(text[2 * i]);
        const int lo = nibble(text[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return Hash256{};
        }
        h.bytes[i] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    return h;
}

Value nullableId(const Uuid& id)
{
    return id == Uuid{} ? Value() : Value::string(id.toString());
}

Result<std::shared_ptr<BusyGuard>> claimOnUi(AppImpl& app, std::string id, JobContext& ctx)
{
    return app.bridge().call<std::shared_ptr<BusyGuard>>(
        [&app, id]() -> Result<std::shared_ptr<BusyGuard>> {
            auto guard = app.claim(id);
            if (!guard) {
                return tl::unexpected<Error>(guard.error());
            }
            return std::make_shared<BusyGuard>(std::move(*guard));
        },
        ctx.cancel);
}

// ---------- cam.batch ----------

constexpr std::string_view kBatch = "cam.batch";
constexpr std::string_view kBatchable[] = {"cam.operation.add",      "cam.operation.edit", "cam.operation.remove",
                                           "cam.operation.reorder",  "cam.operation.suppress",
                                           "cam.library.embed",      "cam.setup.edit"};

class CamBatch final : public doc::ICommandHandler {
public:
    explicit CamBatch(std::shared_ptr<const doc::CommandRegistry> base) : base_(std::move(base))
    {
        Value names = Value::array();
        for (std::string_view n : kBatchable) {
            names.push(Value::string(std::string(n)));
        }
        Value item = Value::object(
            {{"type", Value::string("object")},
             {"additionalProperties", Value::boolean(false)},
             {"required", Value::array({Value::string("name"), Value::string("args")})},
             {"properties", Value::object({{"name", Value::object({{"enum", std::move(names)}})},
                                           {"args", Value::object({{"type", Value::string("object")}})}})}});
        Value commands = Value::object({{"type", Value::string("array")},
                                        {"minItems", Value::integer(1)},
                                        {"items", std::move(item)},
                                        {"description", Value::string("The CAM commands, applied in this order as "
                                                                      "one change (one undo step)")}});
        schema_ = schemaOf(Value::object({{"commands", std::move(commands)}}), {"commands"});
    }
    std::string_view name() const override { return kBatch; }
    std::string_view argsSchemaJson() const override { return schema_; }
    bool changesToolpathsOrGcode() const override { return true; }
    Result<doc::DocStatePtr> apply(doc::DocStatePtr before, std::string_view argsJson) const override
    {
        try {
            if (!before) {
                return fail(ErrorCode::InvalidArgument, "cam.batch: there is no document snapshot to apply it to");
            }
            auto args = json::parse(argsJson);
            const Value* list = args && args->isObject() ? args->find("commands") : nullptr;
            if (list == nullptr || !list->isArray() || list->asArray().empty()) {
                return fail(ErrorCode::InvalidArgument, "cam.batch: 'commands' must be a non-empty array");
            }
            if (args->asObject().size() != 1) {
                return fail(ErrorCode::InvalidArgument, "cam.batch: only 'commands' is allowed");
            }
            doc::DocStatePtr state = std::move(before);
            for (std::size_t i = 0; i < list->asArray().size(); ++i) {
                const Value& c = list->asArray()[i];
                const std::string where = "cam.batch: commands[" + std::to_string(i) + "]";
                const Value* name = c.isObject() ? c.find("name") : nullptr;
                const Value* cargs = c.isObject() ? c.find("args") : nullptr;
                if (name == nullptr || !name->isString() || cargs == nullptr || !cargs->isObject() ||
                    c.asObject().size() != 2) {
                    return fail(ErrorCode::InvalidArgument, where + " must be {\"name\": ..., \"args\": {...}}");
                }
                const std::string& n = name->asString();
                if (std::find(std::begin(kBatchable), std::end(kBatchable), n) == std::end(kBatchable)) {
                    return fail(ErrorCode::InvalidArgument, where + ": '" + n + "' cannot be part of a cam.batch");
                }
                const doc::ICommandHandler* handler = base_ ? base_->find(n) : nullptr;
                if (handler == nullptr) {
                    return fail(ErrorCode::Internal, where + ": the command '" + n + "' is not registered");
                }
                auto next = handler->apply(state, json::canonical(*cargs));
                if (!next) {
                    Error e = next.error();
                    e.message = where + " (" + n + "): " + e.message;
                    return tl::unexpected<Error>(std::move(e));
                }
                state = std::move(*next);
            }
            return state;
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::Internal, "cam.batch: out of memory");
        }
    }

private:
    std::shared_ptr<const doc::CommandRegistry> base_;
    std::string schema_;
};

// ---------- The document's expressions ----------

tpl::EvaluateFn evaluatorFor(const doc::DocState& state)
{
    auto params = std::make_shared<std::map<std::string, double>>();
    if (auto evaluated = doc::evaluateParameters(state.parameters)) {
        *params = std::move(*evaluated);
    }
    return [params](std::string_view expression, tpl::FieldKind kind) -> Result<double> {
        const doc::Parameter::Kind k = kind == tpl::FieldKind::Length  ? doc::Parameter::Kind::Length
                                       : kind == tpl::FieldKind::Angle ? doc::Parameter::Kind::Angle
                                       : kind == tpl::FieldKind::Count ? doc::Parameter::Kind::Count
                                                                       : doc::Parameter::Kind::Unitless;
        auto v = doc::evaluate(expression, *params, k);
        if (v && kind == tpl::FieldKind::Angle) {
            return *v * 57.29577951308232; // radians to degrees, as templates and schemas give angles
        }
        return v;
    };
}

// ---------- The holes of a setup ----------

const cam::Setup* findSetup(const cam::CamState& cam, const SetupId& id)
{
    const auto it = std::find_if(cam.setups.begin(), cam.setups.end(), [&](const cam::Setup& s) { return s.id == id; });
    return it == cam.setups.end() ? nullptr : &*it;
}

struct SetupHoles {
    std::vector<tpl::PlanHole> holes;
    std::vector<BodyId> bodies; // parallel to holes
    std::vector<tpl::HoleGroup> groups;
};

Result<SetupHoles> setupHoles(const doc::DocStatePtr& state, const doc::BuildResultPtr& build, const SetupId& id,
                              JobContext& ctx)
{
    if (!state || !build) {
        return fail(ErrorCode::InvalidArgument, "The model is not built yet; try again when the rebuild ends");
    }
    const cam::CamSnapshot snap = doc::camSnapshot(state, build);
    const cam::Setup* setup = snap.cam ? findSetup(*snap.cam, id) : nullptr;
    if (setup == nullptr) {
        return fail(ErrorCode::NotFound, "The document has no setup " + id.value.toString());
    }
    const auto resolver = naming::makeResolver(snap.bodies);
    if (!resolver) {
        return fail(ErrorCode::Internal, "Out of memory");
    }
    auto resolved = cam::resolveSetup(snap, *setup, *resolver, ctx);
    if (!resolved) {
        return tl::unexpected(resolved.error());
    }
    cam::ResolvedGeometry geom{};
    geom.wcsFromModel = resolved->wcsFromModel;
    geom.holeThreads = snap.holeThreads;
    for (const BodyId& b : setup->partBodies) {
        const auto it = build->bodies.find(b);
        if (it == build->bodies.end()) {
            return fail(ErrorCode::ReferenceBroken, "A part body of the setup is not built");
        }
        geom.partShapes.push_back(it->second.shape);
        geom.partNames.push_back(it->second.map);
    }
    SetupHoles out;
    std::vector<cam::RecognizedHole> all;
    for (std::size_t i = 0; i < geom.partShapes.size(); ++i) {
        auto holes = cam::recognizeHoles(geom.partShapes[i], *resolver, setup->partBodies[i], geom.wcsFromModel, ctx,
                                         cam::holeOptionsFor(geom, i));
        if (!holes) {
            return tl::unexpected(holes.error());
        }
        for (cam::RecognizedHole& h : *holes) {
            all.push_back(h);
            naming::TopoRef ref = h.axisFace;
            out.holes.push_back(tpl::PlanHole{std::move(h), std::move(ref)});
            out.bodies.push_back(setup->partBodies[i]);
        }
    }
    out.groups = tpl::groupHoles(all);
    return out;
}

// ---------- Requests ----------

struct Request {
    std::string documentId;
    SetupId setup;
    std::vector<Uuid> templates;
    std::string set;
    std::optional<Value> holes;
    std::vector<naming::TopoRef> geometry;
};

Result<Request> readRequest(const Value& args)
{
    Request r;
    r.documentId = args.stringOr("document", "");
    r.setup = SetupId{Uuid::fromString(args.stringOr("setup", ""))};
    if (const Value* list = args.find("templates")) {
        for (const Value& t : list->asArray()) {
            r.templates.push_back(Uuid::fromString(t.asString()));
        }
    }
    r.set = args.stringOr("set", "");
    if (r.templates.empty() && r.set.empty()) {
        return fail(ErrorCode::InvalidArgument, "Name the templates to apply: 'templates' (ids) or 'set'");
    }
    if (!r.templates.empty() && !r.set.empty()) {
        return fail(ErrorCode::InvalidArgument, "Give 'templates' or 'set', not both");
    }
    if (const Value* holes = args.find("holes")) {
        r.holes = *holes;
    }
    if (const Value* geometry = args.find("geometry")) {
        for (std::size_t i = 0; i < geometry->asArray().size(); ++i) {
            auto ref = naming::topoRefFromJson(json::write(geometry->asArray()[i]));
            if (!ref) {
                return fail(ErrorCode::InvalidArgument,
                            "/geometry/" + std::to_string(i) + ": not a reference: " + ref.error().message);
            }
            r.geometry.push_back(std::move(*ref));
        }
        if (r.geometry.empty()) {
            return fail(ErrorCode::InvalidArgument, "/geometry: list at least one face of a hole");
        }
    }
    if (r.holes && !r.geometry.empty()) {
        return fail(ErrorCode::InvalidArgument, "Give 'holes' or 'geometry', not both");
    }
    return r;
}

Value holesFilterSchema()
{
    Value diameter = prop("number", "The bore's diameter in mm (within 0.001 mm)");
    Value kind = prop("string", "'hole', 'hole.tapped', 'hole.plain', 'hole.through', 'hole.blind', "
                                "'hole.counterbored' or 'hole.countersunk'");
    return Value::object(
        {{"type", Value::string("object")},
         {"additionalProperties", Value::boolean(false)},
         {"description", Value::string("Which recognised holes of the setup: those matching every member given")},
         {"properties", Value::object({{"group", prop("string", "A hole group's key (cam.template.preview's groups)")},
                                       {"kind", std::move(kind)},
                                       {"threadSpec", prop("string", "The thread designation, \"M6x1\"")},
                                       {"diameter", std::move(diameter)},
                                       {"through", prop("boolean", "Through holes (true) or blind (false)")}})}});
}

Value requestProperties()
{
    Value templates = Value::object({{"type", Value::string("array")},
                                     {"items", uuidProp("A template id")},
                                     {"description", Value::string("The templates to apply, by id")}});
    Value geometry = Value::object(
        {{"type", Value::string("array")},
         {"items", Value::object({{"type", Value::string("object")}})},
         {"description", Value::string("References (TopoRef JSON) to a face of each hole to apply them to")}});
    return Value::object({{"document", prop("string", "The OpenDocument id; the active document when omitted")},
                          {"setup", uuidProp("The setup whose holes get the operations")},
                          {"templates", std::move(templates)},
                          {"set", prop("string", "A template set's name: its templates for the setup's material")},
                          {"holes", holesFilterSchema()},
                          {"geometry", std::move(geometry)}});
}

bool groupMatches(const tpl::HoleGroup& g, const cam::RecognizedHole& h, const Value& filter)
{
    if (const Value* key = filter.find("group"); key != nullptr && key->asString() != g.key) {
        return false;
    }
    if (const Value* kind = filter.find("kind"); kind != nullptr && !tpl::kindMatches(kind->asString(), h)) {
        return false;
    }
    if (const Value* spec = filter.find("threadSpec"); spec != nullptr && spec->asString() != h.threadSpec) {
        return false;
    }
    if (const Value* d = filter.find("diameter"); d != nullptr && std::fabs(d->asNumber() - g.diameter) > 0.001) {
        return false;
    }
    if (const Value* through = filter.find("through"); through != nullptr && through->asBool() != h.through) {
        return false;
    }
    return true;
}

// The holes the request names, in the setup's hole order.
Result<std::vector<std::size_t>> targetsOf(const SetupHoles& sh, const Request& rq, const naming::IResolver& resolver)
{
    std::vector<std::size_t> out;
    if (!rq.geometry.empty()) {
        std::set<std::size_t> chosen;
        for (std::size_t i = 0; i < rq.geometry.size(); ++i) {
            const naming::Resolved r = resolver.resolve(rq.geometry[i]);
            if (!r.usable()) {
                return fail(ErrorCode::ReferenceBroken, "/geometry/" + std::to_string(i) +
                                                            " does not resolve: " + r.reason);
            }
            bool found = false;
            for (std::size_t h = 0; h < sh.holes.size(); ++h) {
                if (sh.bodies[h] != rq.geometry[i].body) {
                    continue;
                }
                const auto& faces = sh.holes[h].hole.faces;
                for (const kernel::SubId& e : r.elements) {
                    if (std::find(faces.begin(), faces.end(), e) != faces.end()) {
                        chosen.insert(h);
                        found = true;
                    }
                }
            }
            if (!found) {
                return fail(ErrorCode::InvalidArgument,
                            "/geometry/" + std::to_string(i) + " is not a face of a hole of the setup");
            }
        }
        out.assign(chosen.begin(), chosen.end());
        return out;
    }
    for (const tpl::HoleGroup& g : sh.groups) {
        for (const std::size_t h : g.holes) {
            if (!rq.holes || groupMatches(g, sh.holes[h].hole, *rq.holes)) {
                out.push_back(h);
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---------- The templates ----------

Result<std::vector<lib::OpTemplate>> templatesOf(const lib::LibraryStack& stack, const Request& rq,
                                                 const cam::Setup& setup)
{
    std::vector<lib::OpTemplate> out;
    if (!rq.templates.empty()) {
        for (const Uuid& id : rq.templates) {
            const lib::OpTemplate* t = stack.opTemplate(id);
            if (t == nullptr) {
                return fail(ErrorCode::NotFound, "No library has the template " + id.toString());
            }
            out.push_back(*t);
        }
        return out;
    }
    for (const lib::OpTemplate& t : stack.templatesFor("", setup.material, setup.machine)) {
        auto rules = tpl::rulesOf(t);
        if (rules && rules->set == rq.set) {
            out.push_back(t);
        }
    }
    if (out.empty()) {
        bool anyMaterial = false;
        for (const lib::OpTemplate& t : stack.templates()) {
            auto rules = tpl::rulesOf(t);
            anyMaterial = anyMaterial || (rules && rules->set == rq.set);
        }
        if (anyMaterial && setup.material.value == Uuid{}) {
            return fail(ErrorCode::NotFound, "The templates of set '" + rq.set +
                                                 "' are for a stock material, and setup '" + setup.name +
                                                 "' has none: choose its material in the setup");
        }
        if (anyMaterial) {
            const lib::Material* m = stack.material(setup.material);
            return fail(ErrorCode::NotFound, "The libraries have no template of set '" + rq.set + "' for " +
                                                 (m != nullptr ? m->name : std::string("the setup's material")) +
                                                 " and the setup's machine");
        }
        return fail(ErrorCode::NotFound, "The libraries have no template set '" + rq.set + "'");
    }
    return out;
}

// ---------- The plan in the document ----------

struct Planned {
    tpl::Plan plan;
    std::vector<lib::OpTemplate> templates;
    std::vector<tpl::PlanHole> holes;      // the targets
    std::vector<std::size_t> targets;      // their indices in the setup's holes
    std::vector<int> numbers;              // tool number of each planned operation
    std::string message;                   // missing tools, or why nothing would be made
    std::optional<doc::Command> command;   // the cam.batch, when makeCommand and the plan is good
    std::vector<OperationId> ids;          // of the operations it adds
};

// The tool numbers the document gives its tools now (the first operation in program order that uses each).
std::map<ToolId, int> toolNumbers(const cam::CamState* cam)
{
    std::map<ToolId, int> out;
    if (cam == nullptr) {
        return out;
    }
    for (const cam::Setup& s : cam->setups) {
        for (const OperationId& id : s.operations) {
            if (const auto it = cam->operations.find(id); it != cam->operations.end()) {
                out.emplace(it->second.tool.tool, it->second.tool.toolNumber);
            }
        }
    }
    return out;
}

Value cuttingJson(const cam::CuttingData& c)
{
    static const char* const coolants[] = {"off", "flood", "mist", "throughTool", "air"};
    return Value::object({{"rpm", Value::number(c.spindleRpm)},
                          {"clockwise", Value::boolean(c.clockwise)},
                          {"feedCut", Value::number(c.feedCut)},
                          {"feedPlunge", Value::number(c.feedPlunge)},
                          {"feedRamp", Value::number(c.feedRamp)},
                          {"feedLeadIn", Value::number(c.feedLeadIn)},
                          {"feedLeadOut", Value::number(c.feedLeadOut)},
                          {"coolant", Value::string(coolants[static_cast<int>(c.coolant)])}});
}

Value heightsJson(const cam::Heights& h)
{
    return Value::object({{"clearance", Value::number(h.clearance)},
                          {"retract", Value::number(h.retract)},
                          {"feedPlane", Value::number(h.feedPlane)},
                          {"top", Value::number(h.top)},
                          {"bottom", Value::number(h.bottom)}});
}

Value calculatedJson(const cam::CalculatedFeeds& c)
{
    return Value::object({{"tool", Value::string(idText(c.tool.value))},
                          {"material", nullableId(c.material.value)},
                          {"machine", nullableId(c.machine.value)},
                          {"opClass", Value::string(c.opClass)},
                          {"rpm", Value::number(c.spindleRpm)},
                          {"feedCut", Value::number(c.feedCut)},
                          {"feedPlunge", Value::number(c.feedPlunge)},
                          {"feedRamp", Value::number(c.feedRamp)}});
}

// The members of cam.operation.add / edit that a plan sets.
Result<Value> operationMembers(const cam::Operation& op, int number)
{
    auto params = parseValue(op.paramsJson.empty() ? std::string_view("{}") : std::string_view(op.paramsJson));
    if (!params) {
        return tl::unexpected(params.error());
    }
    Value geometry = Value::array();
    for (const naming::TopoRef& ref : op.geometry) {
        auto g = parseValue(naming::toJson(ref));
        if (!g) {
            return tl::unexpected(g.error());
        }
        geometry.push(std::move(*g));
    }
    Value out = Value::object({{"tool", Value::object({{"tool", Value::string(idText(op.tool.tool.value))},
                                                       {"number", Value::integer(number)},
                                                       {"lengthOffset", Value::integer(number)},
                                                       {"diameterOffset", Value::integer(number)}})},
                               {"cutting", cuttingJson(op.cutting)},
                               {"heights", heightsJson(op.heights)},
                               {"geometry", std::move(geometry)},
                               {"params", std::move(*params)},
                               {"template", Value::string(op.templateId)},
                               {"calculatedFeeds", op.calculated ? calculatedJson(*op.calculated) : Value()}});
    return out;
}

// Plans the request on the UI thread (the library stack is the UI thread's). `state`: the document now.
Result<Planned> planOnUi(AppImpl& app, const doc::DocState& state, const SetupHoles& sh, const Request& rq,
                         const std::vector<std::size_t>& targets, bool makeCommand)
{
    lib::LibraryStack* stack = app.libraries();
    if (stack == nullptr) {
        return fail(ErrorCode::Internal, "There is no tool library");
    }
    const cam::Setup* setup = state.cam ? findSetup(*state.cam, rq.setup) : nullptr;
    if (setup == nullptr) {
        return fail(ErrorCode::NotFound, "The document has no setup " + rq.setup.value.toString());
    }
    Planned p;
    auto templates = templatesOf(*stack, rq, *setup);
    if (!templates) {
        return tl::unexpected(templates.error());
    }
    p.templates = std::move(*templates);
    p.targets = targets;
    for (const std::size_t h : targets) {
        p.holes.push_back(sh.holes[h]);
    }

    // The material the feeds are calculated for: the setup's, else the one every template names.
    const lib::Material* material = nullptr;
    std::optional<lib::Material> libraryMaterial;
    if (setup->material.value != Uuid{}) {
        if (const auto it = state.cam->materials.find(setup->material); it != state.cam->materials.end()) {
            material = &it->second.item;
        }
    } else {
        std::set<MaterialId> named;
        for (const lib::OpTemplate& t : p.templates) {
            if (t.material) {
                named.insert(*t.material);
            }
        }
        if (named.size() == 1) {
            if (const lib::Material* m = stack->material(*named.begin())) {
                libraryMaterial = *m;
                material = &*libraryMaterial;
            }
        }
    }
    tpl::PlanContext ctx;
    ctx.queryTools = [stack](std::string_view query) { return stack->queryTools(query, {}); };
    ctx.evaluate = evaluatorFor(state);
    ctx.types = app.operationTypes();
    ctx.material = material;
    if (const auto it = state.cam->machines.find(setup->machine); it != state.cam->machines.end()) {
        ctx.machine = it->second.item;
    }
    const bool inch = state.settings.units == UnitSystem::Inch;
    ctx.defaultHeights = cam::Heights{inch ? 15.24 : 15.0, inch ? 5.08 : 5.0, inch ? 2.54 : 2.0, 0.0, 0.0};
    auto plan = tpl::planTemplates(p.templates, p.holes, ctx);
    if (!plan) {
        return tl::unexpected(plan.error());
    }
    p.plan = std::move(*plan);

    // Tool numbers.
    std::map<ToolId, int> numbers = toolNumbers(state.cam.get());
    std::set<int> taken;
    for (const auto& [tool, n] : numbers) {
        taken.insert(n);
    }
    for (const tpl::PlannedOperation& op : p.plan.operations) {
        auto it = numbers.find(op.tool.id);
        if (it == numbers.end()) {
            int n = op.tool.defaultToolNumber;
            if (n <= 0 || taken.count(n) != 0) {
                n = taken.empty() ? 1 : *taken.rbegin() + 1;
            }
            taken.insert(n);
            it = numbers.emplace(op.tool.id, n).first;
        }
        p.numbers.push_back(it->second);
    }

    if (!p.plan.missing.empty()) {
        p.message = tpl::missingToolsMessage(p.plan, p.templates, p.holes);
    } else if (targets.empty()) {
        p.message = "The setup has no recognised hole that the request names";
    } else if (p.plan.operations.empty()) {
        p.message = "None of the templates applies to the holes named (their feature kind or their conditions do not "
                    "match)";
    }
    if (!makeCommand || !p.message.empty()) {
        return p;
    }

    // The cam.batch: the library items first, then one cam.operation.add per planned operation.
    Value commands = Value::array();
    Value tools = Value::array();
    std::set<ToolId> embedding;
    for (const tpl::PlannedOperation& op : p.plan.operations) {
        if (state.cam->tools.count(op.tool.id) == 0 && embedding.insert(op.tool.id).second) {
            auto item = parseValue(lib::toJson(op.tool));
            if (!item) {
                return tl::unexpected(item.error());
            }
            tools.push(std::move(*item));
        }
    }
    Value library = Value::object();
    if (!tools.asArray().empty()) {
        library.set("tools", std::move(tools));
    }
    if (libraryMaterial && state.cam->materials.count(libraryMaterial->id) == 0) {
        auto item = parseValue(lib::toJson(*libraryMaterial));
        if (!item) {
            return tl::unexpected(item.error());
        }
        library.set("materials", Value::array({std::move(*item)}));
    }
    if (!library.asObject().empty()) {
        commands.push(Value::object({{"name", Value::string("cam.library.embed")}, {"args", std::move(library)}}));
    }
    for (std::size_t i = 0; i < p.plan.operations.size(); ++i) {
        const tpl::PlannedOperation& op = p.plan.operations[i];
        const OperationId id{Uuid::generate()};
        p.ids.push_back(id);
        auto members = operationMembers(op.op, p.numbers[i]);
        if (!members) {
            return tl::unexpected(members.error());
        }
        members->set("id", Value::string(idText(id.value)));
        members->set("setup", Value::string(idText(rq.setup.value)));
        members->set("type", Value::string(op.op.type));
        members->set("name", Value::string(op.op.name));
        // The params completed with the type's defaults, as the cam.operation.add action stores them.
        auto checked = checkOperationParams(app.operationTypes(), state,
                                            doc::Command{"cam.operation.add", json::canonical(*members), ""});
        if (!checked) {
            const std::string name = p.templates[op.templateIndex].name;
            return fail(checked.error().code, "Template '" + name + "': " + checked.error().message);
        }
        auto completed = parseValue(checked->argsJson);
        if (!completed) {
            return tl::unexpected(completed.error());
        }
        commands.push(
            Value::object({{"name", Value::string("cam.operation.add")}, {"args", std::move(*completed)}}));
    }
    std::string label = "Apply Templates";
    if (!rq.set.empty()) {
        label = "Apply Template Set " + rq.set;
    } else if (p.templates.size() == 1) {
        label = "Apply Template " + p.templates.front().name;
    }
    p.command = doc::Command{std::string(kBatch), json::canonical(Value::object({{"commands", std::move(commands)}})),
                             label};
    return p;
}

Value groupsJson(const SetupHoles& sh, const std::vector<std::size_t>& targets)
{
    Value out = Value::array();
    for (const tpl::HoleGroup& g : sh.groups) {
        const bool targeted = std::any_of(g.holes.begin(), g.holes.end(), [&](std::size_t h) {
            return std::binary_search(targets.begin(), targets.end(), h);
        });
        out.push(Value::object({{"key", Value::string(g.key)},
                                {"kind", Value::string(g.kind)},
                                {"threadSpec", Value::string(g.threadSpec)},
                                {"diameter", Value::number(g.diameter)},
                                {"depth", Value::number(g.depth)},
                                {"through", Value::boolean(g.through)},
                                {"counterboreDiameter", Value::number(g.counterboreDiameter)},
                                {"counterboreDepth", Value::number(g.counterboreDepth)},
                                {"countersinkDiameter", Value::number(g.countersinkDiameter)},
                                {"count", Value::integer(static_cast<std::int64_t>(g.holes.size()))},
                                {"label", Value::string(g.label)},
                                {"targeted", Value::boolean(targeted)}}));
    }
    return out;
}

Value planJson(const SetupHoles& sh, const Planned& p, const SetupId& setup)
{
    Value templates = Value::array();
    for (const lib::OpTemplate& t : p.templates) {
        auto rules = tpl::rulesOf(t);
        templates.push(Value::object({{"id", Value::string(idText(t.id))},
                                      {"name", Value::string(t.name)},
                                      {"set", Value::string(rules ? rules->set : std::string())},
                                      {"sequence", Value::integer(rules ? rules->sequence : 0)}}));
    }
    Value ops = Value::array();
    for (std::size_t i = 0; i < p.plan.operations.size(); ++i) {
        const tpl::PlannedOperation& op = p.plan.operations[i];
        auto params = json::parse(op.op.paramsJson);
        Value o = Value::object({{"name", Value::string(op.op.name)},
                                 {"type", Value::string(op.op.type)},
                                 {"template", Value::string(op.op.templateId)},
                                 {"templateName", Value::string(p.templates[op.templateIndex].name)},
                                 {"tool", Value::string(idText(op.tool.id.value))},
                                 {"toolName", Value::string(op.tool.name)},
                                 {"toolNumber", Value::integer(p.numbers[i])},
                                 {"holes", Value::integer(static_cast<std::int64_t>(op.holes.size()))},
                                 {"params", params ? std::move(*params) : Value::object()},
                                 {"rpm", Value::number(op.op.cutting.spindleRpm)},
                                 {"feedCut", Value::number(op.op.cutting.feedCut)}});
        if (i < p.ids.size()) {
            o.set("id", Value::string(idText(p.ids[i].value)));
        }
        ops.push(std::move(o));
    }
    Value missing = Value::array();
    for (const tpl::MissingTool& m : p.plan.missing) {
        Value where = Value::array();
        for (const std::size_t h : m.holes) {
            where.push(Value::string(tpl::describeHole(p.holes[h].hole)));
        }
        missing.push(Value::object({{"template", Value::string(idText(p.templates[m.templateIndex].id))},
                                    {"templateName", Value::string(p.templates[m.templateIndex].name)},
                                    {"rule", Value::string(m.rule)},
                                    {"holes", Value::integer(static_cast<std::int64_t>(m.holes.size()))},
                                    {"where", std::move(where)}}));
    }
    return Value::object({{"setup", Value::string(idText(setup.value))},
                          {"holes", Value::integer(static_cast<std::int64_t>(sh.holes.size()))},
                          {"groups", groupsJson(sh, p.targets)},
                          {"targets", Value::integer(static_cast<std::int64_t>(p.targets.size()))},
                          {"templates", std::move(templates)},
                          {"operations", std::move(ops)},
                          {"missing", std::move(missing)},
                          {"untouched", Value::integer(static_cast<std::int64_t>(p.plan.untouched.size()))},
                          {"ok", Value::boolean(p.message.empty())},
                          {"message", Value::string(p.message)}});
}

// preview and apply: the holes on this worker, the plan (and the change) on the UI thread.
Result<ActionResult> applyAction(AppImpl& app, std::string_view argsJson, bool apply, JobContext& ctx)
{
    auto args = parseArgs(argsJson);
    if (!args) {
        return tl::unexpected(args.error());
    }
    auto rq = readRequest(*args);
    if (!rq) {
        return tl::unexpected(rq.error());
    }
    auto guard = claimOnUi(app, rq->documentId, ctx);
    if (!guard) {
        return tl::unexpected(guard.error());
    }
    DocEntry& entry = *(*guard)->get();
    const doc::DocStatePtr state = entry.od.document->state();
    const doc::BuildResultPtr build = entry.od.document->build();
    auto holes = setupHoles(state, build, rq->setup, ctx);
    if (!holes) {
        return tl::unexpected(holes.error());
    }
    std::vector<std::size_t> targets;
    {
        const cam::CamSnapshot snap = doc::camSnapshot(state, build);
        const auto resolver = naming::makeResolver(snap.bodies);
        if (!resolver) {
            return fail(ErrorCode::Internal, "Out of memory");
        }
        auto t = targetsOf(*holes, *rq, *resolver);
        if (!t) {
            return tl::unexpected(t.error());
        }
        targets = std::move(*t);
    }
    return app.bridge().call<ActionResult>(
        [&]() -> Result<ActionResult> {
            const doc::DocStatePtr now = entry.od.document->state();
            auto planned = planOnUi(app, *now, *holes, *rq, targets, apply);
            if (!planned) {
                return tl::unexpected(planned.error());
            }
            if (!apply) {
                return result(planJson(*holes, *planned, rq->setup));
            }
            if (!planned->message.empty()) {
                std::string message = planned->message;
                message[0] = static_cast<char>(message[0] - 'A' + 'a');
                return fail(ErrorCode::NotFound, "Nothing was created: " + message);
            }
            if (!app.pointOfNoReturn(ctx.cancel)) {
                return fail(ErrorCode::Cancelled, "Cancelled");
            }
            auto executed = app.execute(entry, *planned->command);
            if (!executed) {
                return tl::unexpected(executed.error());
            }
            return result(planJson(*holes, *planned, rq->setup));
        },
        ctx.cancel);
}

// ---------- re-apply ----------

struct OpContext {
    const cam::Operation* op = nullptr;
    const cam::Setup* setup = nullptr;
};
OpContext findOperation(const doc::DocState& state, const OperationId& id)
{
    OpContext out;
    if (!state.cam) {
        return out;
    }
    const auto it = state.cam->operations.find(id);
    if (it == state.cam->operations.end()) {
        return out;
    }
    out.op = &it->second;
    for (const cam::Setup& s : state.cam->setups) {
        if (std::find(s.operations.begin(), s.operations.end(), id) != s.operations.end()) {
            out.setup = &s;
        }
    }
    return out;
}

// The holes (indices into sh) an operation's geometry names.
Result<std::vector<std::size_t>> holesOfOperation(const SetupHoles& sh, const cam::Operation& op,
                                                  const naming::IResolver& resolver)
{
    Request rq;
    rq.geometry = op.geometry;
    if (rq.geometry.empty()) {
        return fail(ErrorCode::InvalidArgument, "Operation '" + op.name + "' has no holes to re-apply its template to");
    }
    return targetsOf(sh, rq, resolver);
}

Result<ActionResult> reapplyAction(AppImpl& app, std::string_view argsJson, JobContext& ctx)
{
    auto args = parseArgs(argsJson);
    if (!args) {
        return tl::unexpected(args.error());
    }
    const OperationId opId{Uuid::fromString(args->stringOr("operation", ""))};
    auto guard = claimOnUi(app, args->stringOr("document", ""), ctx);
    if (!guard) {
        return tl::unexpected(guard.error());
    }
    DocEntry& entry = *(*guard)->get();
    const doc::DocStatePtr state = entry.od.document->state();
    const doc::BuildResultPtr build = entry.od.document->build();
    const OpContext oc = findOperation(*state, opId);
    if (oc.op == nullptr || oc.setup == nullptr) {
        return fail(ErrorCode::NotFound, "The document has no operation " + opId.value.toString());
    }
    const Uuid templateId = Uuid::fromString(oc.op->templateId);
    if (templateId == Uuid{}) {
        return fail(ErrorCode::InvalidArgument,
                    "Operation '" + oc.op->name + "' was not made from a template, so there is none to re-apply");
    }
    auto holes = setupHoles(state, build, oc.setup->id, ctx);
    if (!holes) {
        return tl::unexpected(holes.error());
    }
    std::vector<std::size_t> targets;
    {
        const cam::CamSnapshot snap = doc::camSnapshot(state, build);
        const auto resolver = naming::makeResolver(snap.bodies);
        if (!resolver) {
            return fail(ErrorCode::Internal, "Out of memory");
        }
        auto t = holesOfOperation(*holes, *oc.op, *resolver);
        if (!t) {
            return tl::unexpected(t.error());
        }
        targets = std::move(*t);
    }
    const SetupId setupId = oc.setup->id;
    const std::string opName = oc.op->name;
    return app.bridge().call<ActionResult>(
        [&]() -> Result<ActionResult> {
            const doc::DocStatePtr now = entry.od.document->state();
            const OpContext current = findOperation(*now, opId);
            if (current.op == nullptr) {
                return fail(ErrorCode::NotFound, "The operation was deleted meanwhile");
            }
            Request rq;
            rq.setup = setupId;
            rq.templates = {templateId};
            auto planned = planOnUi(app, *now, *holes, rq, targets, false);
            if (!planned) {
                if (planned.error().code == ErrorCode::NotFound) {
                    return fail(ErrorCode::NotFound, "The template operation '" + opName +
                                                         "' came from is not in the libraries any more (" +
                                                         templateId.toString() + ")");
                }
                return tl::unexpected(planned.error());
            }
            if (!planned->message.empty()) {
                return fail(ErrorCode::NotFound, planned->message);
            }
            if (planned->plan.operations.size() != 1 ||
                planned->plan.operations.front().holes.size() != targets.size()) {
                return fail(ErrorCode::InvalidArgument,
                            "Re-applying template '" + planned->templates.front().name + "' to operation '" + opName +
                                "' would not give one operation for all its holes (" +
                                std::to_string(planned->plan.operations.size()) +
                                " operations); apply the template to the holes again instead");
            }
            const tpl::PlannedOperation& op = planned->plan.operations.front();
            // Keep the operation's own tool number when the tool stays.
            int number = planned->numbers.front();
            if (current.op->tool.tool == op.tool.id) {
                number = current.op->tool.toolNumber;
            }
            auto members = operationMembers(op.op, number);
            if (!members) {
                return tl::unexpected(members.error());
            }
            members->erase("geometry"); // the operation keeps its holes
            members->set("id", Value::string(idText(opId.value)));
            members->set("type", Value::string(op.op.type));
            if (now->cam->tools.count(op.tool.id) == 0) {
                auto item = parseValue(lib::toJson(op.tool));
                if (!item) {
                    return tl::unexpected(item.error());
                }
                members->set("library", Value::object({{"tools", Value::array({std::move(*item)})}}));
            }
            auto checked = checkOperationParams(app.operationTypes(), *now,
                                                doc::Command{"cam.operation.edit", json::canonical(*members), ""});
            if (!checked) {
                return tl::unexpected(checked.error());
            }
            if (!app.pointOfNoReturn(ctx.cancel)) {
                return fail(ErrorCode::Cancelled, "Cancelled");
            }
            auto executed = app.execute(entry, doc::Command{"cam.operation.edit", checked->argsJson,
                                                            "Re-apply Template " + planned->templates.front().name});
            if (!executed) {
                return tl::unexpected(executed.error());
            }
            Value out = planJson(*holes, *planned, setupId);
            out.set("operation", Value::string(idText(opId.value)));
            return result(out);
        },
        ctx.cancel);
}

// ---------- saveFrom, rename, remove ----------

// The writable root to save a new template into: the user's own library (the last writable root).
std::optional<std::size_t> userRoot(const lib::LibraryStack& stack)
{
    std::optional<std::size_t> out;
    const auto roots = stack.roots();
    for (std::size_t i = 0; i < roots.size(); ++i) {
        if (roots[i].writable) {
            out = i;
        }
    }
    return out;
}

std::string mmText(double mm)
{
    // Shortest round-trip text, with a leading "+ " or "- " for an offset.
    std::string s = json::write(Value::number(std::fabs(mm)));
    return s;
}

// The tool rule of a saved template: the tool's type and its diameter's relation to the hole.
Value toolRule(const lib::Tool& tool, std::string_view cycle, const cam::RecognizedHole* hole)
{
    const std::string type(cam_tools::toolTypeName(tool.type));
    Value q = Value::object({{"type", Value::string(type)}});
    if (tool.type == lib::ToolType::Tap) {
        if (hole != nullptr && !hole->threadSpec.empty()) {
            q.set("threadSpec", Value::string("$feature.threadSpec"));
        } else {
            q.set("threadSpec", Value::string(tool.threadSpec));
        }
        return q;
    }
    if (hole == nullptr) {
        q.set("diameter", Value::number(tool.diameter));
        return q;
    }
    const tpl::Vars fv = tpl::featureVars(*hole);
    const double d = fv.at("feature.diameter").number;
    if (cycle == "spot" ||
        ((tool.type == lib::ToolType::SpotDrill || tool.type == lib::ToolType::ChamferMill) && tool.diameter > d)) {
        q.set("diameter", Value::object({{"min", Value::string("$feature.spotDiameter")}}));
        return q;
    }
    const double delta = tool.diameter - d;
    const std::string offset = std::fabs(delta) < 1e-9 ? std::string()
                               : delta > 0               ? " + " + mmText(delta) + " mm"
                                                         : " - " + mmText(delta) + " mm";
    const double window = std::fabs(delta) <= 0.03 ? 0.03 : 0.05;
    const std::string target = "$feature.diameter" + offset;
    q.set("diameter", Value::object({{"min", Value::string(target + " - " + mmText(window) + " mm")},
                                     {"max", Value::string(target + " + " + mmText(window) + " mm")}}));
    q.set("prefer", Value::object({{"diameter", Value::string(target)}}));
    return q;
}

Result<ActionResult> saveFromAction(AppImpl& app, std::string_view argsJson, JobContext& ctx)
{
    auto args = parseArgs(argsJson);
    if (!args) {
        return tl::unexpected(args.error());
    }
    const OperationId opId{Uuid::fromString(args->stringOr("operation", ""))};
    auto guard = claimOnUi(app, args->stringOr("document", ""), ctx);
    if (!guard) {
        return tl::unexpected(guard.error());
    }
    DocEntry& entry = *(*guard)->get();
    const doc::DocStatePtr state = entry.od.document->state();
    const doc::BuildResultPtr build = entry.od.document->build();
    const OpContext oc = findOperation(*state, opId);
    if (oc.op == nullptr || oc.setup == nullptr) {
        return fail(ErrorCode::NotFound, "The document has no operation " + opId.value.toString());
    }
    const cam::Operation& op = *oc.op;
    const auto tool = state->cam->tools.find(op.tool.tool);
    if (tool == state->cam->tools.end()) {
        return fail(ErrorCode::NotFound, "Operation '" + op.name + "' has no tool to make a tool rule from");
    }
    // The operation's holes (the first one sets the tool rule's relation; all of them the feature kind).
    std::vector<cam::RecognizedHole> opHoles;
    if (!op.geometry.empty() && build) {
        auto holes = setupHoles(state, build, oc.setup->id, ctx);
        if (holes) {
            const cam::CamSnapshot snap = doc::camSnapshot(state, build);
            const auto resolver = naming::makeResolver(snap.bodies);
            if (resolver) {
                if (auto t = holesOfOperation(*holes, op, *resolver)) {
                    for (const std::size_t h : *t) {
                        opHoles.push_back(holes->holes[h].hole);
                    }
                }
            }
        }
    }

    lib::OpTemplate t;
    const std::string givenId = args->stringOr("id", "");
    t.id = givenId.empty() ? Uuid::generate() : Uuid::fromString(givenId);
    t.name = args->stringOr("name", op.name);
    if (t.name.empty()) {
        return fail(ErrorCode::InvalidArgument, "A template needs a name");
    }
    t.opType = op.type;
    if (const Value* kind = args->find("featureKind")) {
        t.featureKind = kind->asString();
    } else if (!opHoles.empty()) {
        const bool allTapped = std::all_of(opHoles.begin(), opHoles.end(), [](const auto& h) {
            return !h.threadSpec.empty();
        });
        const bool nonTapped = std::none_of(opHoles.begin(), opHoles.end(), [](const auto& h) {
            return !h.threadSpec.empty();
        });
        t.featureKind = allTapped ? "hole.tapped" : nonTapped ? "hole.plain" : "hole";
    }
    // Material: the setup's unless the arguments say otherwise (null: any material).
    if (const Value* m = args->find("material")) {
        if (m->isString() && m->asString() != "setup") {
            t.material = MaterialId{Uuid::fromString(m->asString())};
        } else if (m->isString() && oc.setup->material.value != Uuid{}) {
            t.material = oc.setup->material;
        }
    } else if (oc.setup->material.value != Uuid{}) {
        t.material = oc.setup->material;
    }
    // Tool rule.
    auto params = parseValue(op.paramsJson.empty() ? std::string_view("{}") : std::string_view(op.paramsJson));
    if (!params) {
        return tl::unexpected(params.error());
    }
    const std::string cycle = params->stringOr("cycle", "");
    const std::string toolMode = args->stringOr("tool", "rule");
    Value query;
    if (toolMode == "exact") {
        query = Value::object({{"type", Value::string(std::string(cam_tools::toolTypeName(tool->second.item.type)))},
                               {"id", Value::string(idText(op.tool.tool.value))}});
    } else {
        query = toolRule(tool->second.item, cycle, opHoles.empty() ? nullptr : &opHoles.front());
    }
    t.toolQueryJson = json::canonical(query);
    t.paramsJson = json::canonical(*params);
    // Feeds and heights rules.
    tpl::TemplateRules rules;
    rules.set = args->stringOr("set", "");
    rules.sequence = static_cast<int>(args->numberOr("sequence", 0));
    const std::string feedsMode = args->stringOr("feeds", op.calculated ? "calculated" : "fixed");
    rules.feeds.calculated = feedsMode == "calculated";
    rules.feeds.coolant = op.cutting.coolant;
    rules.feeds.clockwise = op.cutting.clockwise;
    if (rules.feeds.calculated) {
        rules.feeds.opClass = op.calculated ? op.calculated->opClass : std::string();
    } else {
        rules.feeds.fixed["rpm"] = json::write(Value::number(op.cutting.spindleRpm));
        rules.feeds.fixed["feedCut"] = json::write(Value::number(op.cutting.feedCut));
        rules.feeds.fixed["feedPlunge"] = json::write(Value::number(op.cutting.feedPlunge));
        rules.feeds.fixed["feedRamp"] = json::write(Value::number(op.cutting.feedRamp));
    }
    if (args->stringOr("heights", "operation") == "operation") {
        const std::pair<const char*, double> heights[] = {{"clearance", op.heights.clearance},
                                                          {"retract", op.heights.retract},
                                                          {"feedPlane", op.heights.feedPlane},
                                                          {"top", op.heights.top},
                                                          {"bottom", op.heights.bottom}};
        for (const auto& [key, value] : heights) {
            rules.heights[key] = json::write(Value::number(value));
        }
    }
    t.extraJson = tpl::extraJsonOf(rules, "");
    t.comment = "Saved from operation '" + op.name + "'.";
    // The template must read back as one (the library checks its schema when it saves).
    if (auto back = tpl::rulesOf(t); !back) {
        return tl::unexpected(back.error());
    }

    // The library to save into and what it holds now (the conflict check), on the UI thread.
    struct Target {
        std::shared_ptr<lib::ILibrary> library;
        std::string name;
        std::optional<Hash256> base;
    };
    const std::optional<std::string> baseText =
        args->find("base") != nullptr ? std::optional<std::string>(args->stringOr("base", "")) : std::nullopt;
    auto target = app.bridge().call<Target>(
        [&]() -> Result<Target> {
            lib::LibraryStack* stack = app.libraries();
            if (stack == nullptr) {
                return fail(ErrorCode::Internal, "There is no library");
            }
            Target out;
            std::optional<std::size_t> root = stack->rootOf(lib::ItemKind::Template, t.id);
            if (root && stack->library(*root) == nullptr) {
                root = userRoot(*stack); // a built-in template: the copy goes into the user's library
            }
            if (!root) {
                root = userRoot(*stack);
            }
            if (!root) {
                return fail(ErrorCode::NotFound, "There is no writable library to save the template in (set "
                                                 "library.userFolder)");
            }
            if (const Value* r = args->find("library"); r != nullptr && r->isNumber()) {
                root = static_cast<std::size_t>(r->asNumber());
                if (stack->library(*root) == nullptr) {
                    return fail(ErrorCode::InvalidArgument, "Library " + std::to_string(*root) + " is not writable");
                }
            }
            out.library = stack->sharedLibrary(*root);
            out.name = stack->roots()[*root].name;
            // What the editor started from: the given hash, else what the library holds now.
            if (baseText) {
                if (!baseText->empty()) {
                    out.base = hashFromHex(*baseText);
                }
            } else {
                for (const lib::OpTemplate& x : out.library->templates()) {
                    if (x.id == t.id) {
                        out.base = cam_tools::contentHash(x);
                    }
                }
            }
            return out;
        },
        ctx.cancel);
    if (!target) {
        return tl::unexpected(target.error());
    }
    if (!ctx.cancel.close()) { // the point of no return: the file is written next
        return fail(ErrorCode::Cancelled, "Cancelled");
    }
    if (auto st = cam_tools::saveTemplate(*target->library, t, target->base); !st) {
        return tl::unexpected(st.error());
    }
    (void)app.bridge().call<bool>(
        [&]() -> Result<bool> {
            if (lib::LibraryStack* stack = app.libraries()) {
                stack->remerge();
            }
            return true;
        },
        CancelToken{});
    auto item = parseValue(lib::toJson(t));
    if (!item) {
        return tl::unexpected(item.error());
    }
    return result(Value::object({{"id", Value::string(idText(t.id))},
                                 {"name", Value::string(t.name)},
                                 {"library", Value::string(target->name)},
                                 {"template", std::move(*item)},
                                 {"hash", Value::string(cam_tools::contentHash(t).hex())}}));
}

// rename and remove: a template of a writable library.
Result<ActionResult> editTemplateAction(AppImpl& app, std::string_view argsJson, bool remove, JobContext& ctx)
{
    auto args = parseArgs(argsJson);
    if (!args) {
        return tl::unexpected(args.error());
    }
    const Uuid id = Uuid::fromString(args->stringOr("id", ""));
    const std::string newName = args->stringOr("name", "");
    if (!remove && newName.empty()) {
        return fail(ErrorCode::InvalidArgument, "A template needs a name");
    }
    struct Target {
        std::shared_ptr<lib::ILibrary> library;
        lib::OpTemplate item;
        Hash256 base;
    };
    const std::optional<std::string> baseText =
        args->find("base") != nullptr ? std::optional<std::string>(args->stringOr("base", "")) : std::nullopt;
    auto target = app.bridge().call<Target>(
        [&]() -> Result<Target> {
            lib::LibraryStack* stack = app.libraries();
            const std::optional<std::size_t> root =
                stack != nullptr ? stack->rootOf(lib::ItemKind::Template, id) : std::nullopt;
            if (!root) {
                return fail(ErrorCode::NotFound, "No library has the template " + id.toString());
            }
            const lib::OpTemplate* t = stack->opTemplate(id);
            if (stack->library(*root) == nullptr || t == nullptr) {
                return fail(ErrorCode::InvalidArgument,
                            "Template '" + (t != nullptr ? t->name : id.toString()) + "' is in " +
                                stack->roots()[*root].name +
                                ", which is read-only: save a copy of it to your library to change it");
            }
            Target out{stack->sharedLibrary(*root), *t, cam_tools::contentHash(*t)};
            if (baseText) {
                out.base = hashFromHex(*baseText);
            }
            return out;
        },
        ctx.cancel);
    if (!target) {
        return tl::unexpected(target.error());
    }
    if (!ctx.cancel.close()) { // the point of no return: the file is written or deleted next
        return fail(ErrorCode::Cancelled, "Cancelled");
    }
    Status st;
    if (remove) {
        st = cam_tools::removeTemplate(*target->library, id, target->base);
    } else {
        target->item.name = newName;
        st = cam_tools::saveTemplate(*target->library, target->item, target->base);
    }
    if (!st) {
        return tl::unexpected(st.error());
    }
    (void)app.bridge().call<bool>(
        [&]() -> Result<bool> {
            if (lib::LibraryStack* stack = app.libraries()) {
                stack->remerge();
            }
            return true;
        },
        CancelToken{});
    Value out = Value::object({{"id", Value::string(idText(id))}});
    if (!remove) {
        out.set("name", Value::string(newName));
        out.set("hash", Value::string(cam_tools::contentHash(target->item).hex()));
    }
    return result(out);
}

} // namespace

std::unique_ptr<doc::ICommandHandler> makeCamBatchCommand(std::shared_ptr<const doc::CommandRegistry> base)
{
    return std::make_unique<CamBatch>(std::move(base));
}

Result<doc::Command> checkBatchParams(const cam::OperationRegistry* types, const doc::DocState& state,
                                      const doc::Command& command)
{
    auto args = json::parse(command.argsJson);
    Value* list = args && args->isObject() ? args->find("commands") : nullptr;
    if (list == nullptr || !list->isArray()) {
        return command; // the command reports it
    }
    for (std::size_t i = 0; i < list->asArray().size(); ++i) {
        Value& c = list->asArray()[i];
        const Value* name = c.isObject() ? c.find("name") : nullptr;
        Value* cargs = c.isObject() ? c.find("args") : nullptr;
        if (name == nullptr || !name->isString() || cargs == nullptr ||
            (name->asString() != "cam.operation.add" && name->asString() != "cam.operation.edit")) {
            continue;
        }
        auto checked = checkOperationParams(types, state, doc::Command{name->asString(), json::canonical(*cargs), ""});
        if (!checked) {
            Error e = checked.error();
            e.message = "cam.batch: commands[" + std::to_string(i) + "]: " + e.message;
            return tl::unexpected<Error>(std::move(e));
        }
        auto completed = json::parse(checked->argsJson);
        if (completed) {
            *cargs = std::move(*completed);
        }
    }
    return doc::Command{command.name, json::canonical(*args), command.label};
}

Status registerCamTemplateActions(AppImpl& app)
{
    // Preview: changes nothing (no confirmation for automation). Runs on a worker: hole recognition is kernel work.
    {
        ActionInfo info;
        info.id = "cam.template.preview";
        info.title = "Preview Templates";
        info.category = "CAM";
        info.description = "Shows what applying operation templates to a setup's holes would create: the operations, "
                           "their tools and hole counts, or which tools are missing; changes nothing";
        info.argsSchemaJson = schemaOf(requestProperties(), {"setup"});
        app.runOnWorker(info.id);
        if (auto st = app.actions().add(std::move(info), [&app](std::string_view a, Caller, JobContext& ctx) {
                return applyAction(app, a, false, ctx);
            });
            !st) {
            return st;
        }
    }
    {
        ActionInfo info;
        info.id = "cam.template.apply";
        info.title = "Apply Templates";
        info.category = "CAM";
        info.description = "Applies operation templates (a set such as 'Tapped holes', or templates by id) to the "
                           "setup's recognised holes, a kind of them or the holes picked: ordinary operations, one undo "
                           "step; refused, naming what is missing, when a tool is not in the libraries";
        info.argsSchemaJson = schemaOf(requestProperties(), {"setup"});
        info.changesDocument = true;
        info.changesToolpathsOrGcode = true;
        app.runOnWorker(info.id);
        app.setOrder(info.id, ActionOrder::Ordered);
        if (auto st = app.actions().add(std::move(info), [&app](std::string_view a, Caller, JobContext& ctx) {
                return applyAction(app, a, true, ctx);
            });
            !st) {
            return st;
        }
    }
    {
        ActionInfo info;
        info.id = "cam.template.reapply";
        info.title = "Re-apply Template";
        info.category = "CAM";
        info.description = "Applies the template an operation came from to the operation's holes again: its tool, "
                           "cutting data, heights and parameters as the template gives them now (one undo step)";
        info.argsSchemaJson = schemaOf(
            Value::object({{"document", prop("string", "The OpenDocument id; the active document when omitted")},
                           {"operation", uuidProp("The operation")}}),
            {"operation"});
        info.changesDocument = true;
        info.changesToolpathsOrGcode = true;
        app.runOnWorker(info.id);
        app.setOrder(info.id, ActionOrder::Ordered);
        if (auto st = app.actions().add(std::move(info), [&app](std::string_view a, Caller, JobContext& ctx) {
                return reapplyAction(app, a, ctx);
            });
            !st) {
            return st;
        }
    }
    {
        Value tool = prop("string", "'rule': a tool query by the tool's type and its diameter's relation to the hole "
                                    "(a tap by the hole's thread); 'exact': this very tool");
        tool.set("enum", Value::array({Value::string("rule"), Value::string("exact")}));
        Value feeds = prop("string", "'calculated': the feeds and speeds calculator at each use; 'fixed': the "
                                     "operation's rpm and feeds. Default: calculated when the operation's feeds were");
        feeds.set("enum", Value::array({Value::string("calculated"), Value::string("fixed")}));
        Value heights = prop("string", "'operation': the operation's heights; 'defaults': the operation panel's");
        heights.set("enum", Value::array({Value::string("operation"), Value::string("defaults")}));
        Value material = Value::object(
            {{"description",
              Value::string("The material the template is for: a material id, \"setup\" (the setup's, the default) or "
                            "null (any)")},
             {"anyOf", Value::array({Value::object({{"type", Value::string("string")}}),
                                     Value::object({{"type", Value::string("null")}})})}});
        Value library = prop("integer", "The library root to save into; the user's own library by default");
        library.set("minimum", Value::integer(0));
        Value sequence = prop("integer", "Its place in its set");
        ActionInfo info;
        info.id = "cam.template.saveFrom";
        info.title = "Save Operation as Template";
        info.category = "CAM";
        info.description = "Saves an operation as an operation template in your library: its type and parameters, a "
                           "tool rule, a feeds rule and its heights";
        info.argsSchemaJson = schemaOf(
            Value::object({{"document", prop("string", "The OpenDocument id; the active document when omitted")},
                           {"operation", uuidProp("The operation to save")},
                           {"name", prop("string", "The template's name; the operation's when omitted")},
                           {"id", uuidProp("Overwrite this template (else a new one)")},
                           {"base", prop("string", "The template's content hash as it was shown (a conflict check)")},
                           {"featureKind", prop("string", "What it is for: 'hole.tapped', 'hole.plain', ...; from "
                                                          "the operation's holes when omitted")},
                           {"material", std::move(material)},
                           {"tool", std::move(tool)},
                           {"feeds", std::move(feeds)},
                           {"heights", std::move(heights)},
                           {"set", prop("string", "The template set it joins")},
                           {"sequence", std::move(sequence)},
                           {"library", std::move(library)}}),
            {"operation"});
        app.runOnWorker(info.id);
        if (auto st = app.actions().add(std::move(info), [&app](std::string_view a, Caller, JobContext& ctx) {
                return saveFromAction(app, a, ctx);
            });
            !st) {
            return st;
        }
    }
    for (const bool remove : {false, true}) {
        ActionInfo info;
        info.id = remove ? "cam.template.remove" : "cam.template.rename";
        info.title = remove ? "Delete Template" : "Rename Template";
        info.category = "CAM";
        info.description = remove ? "Deletes an operation template from its library (not a built-in one)"
                                  : "Renames an operation template of a writable library";
        Value props = Value::object({{"id", uuidProp("The template")},
                                     {"base", prop("string", "The template's content hash as it was shown")}});
        std::vector<std::string> required = {"id"};
        if (!remove) {
            Value name = prop("string", "The new name");
            name.set("minLength", Value::integer(1));
            props.set("name", std::move(name));
            required.push_back("name");
        }
        info.argsSchemaJson = schemaOf(std::move(props), std::move(required));
        app.runOnWorker(info.id);
        if (auto st = app.actions().add(std::move(info), [&app, remove](std::string_view a, Caller, JobContext& ctx) {
                return editTemplateAction(app, a, remove, ctx);
            });
            !st) {
            return st;
        }
    }
    return {};
}

} // namespace ray::api::detail
