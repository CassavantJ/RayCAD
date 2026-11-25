// The built-in actions: app.*, file.*, edit.undo/redo, and one action per registered document command.
//
// Actions that name a document take an optional "document" argument (OpenDocument::id); without it they act on the
// active document. Results are canonical JSON objects.
#include "app_impl.h"
#include "cam_post_actions.h"
#include "cam_template_actions.h"
#include "measure_actions.h"
#include "schema.h"

#include "../doc/features/hole_standards.h"

#include <ray_api/cam_operation.h>
#include <ray_api/feature_edit.h>

#include <optional>
#include <string>
#include <utility>

namespace ray::api::detail {
namespace {

using json::Value;

constexpr const char* kUuidPattern = "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$";

Value prop(const char* type, const char* description)
{
    return Value::object({{"type", Value::string(type)}, {"description", Value::string(description)}});
}

Value documentProp()
{
    return prop("string", "The OpenDocument id; the active document when omitted");
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

ActionResult result(Value v)
{
    return ActionResult{json::canonical(v), {}};
}

Value documentResult(const OpenDocument& od)
{
    Value out = Value::object();
    out.set("document", Value::string(od.id));
    out.set("title", Value::string(od.title));
    out.set("path", Value::string(od.path));
    out.set("readOnly", Value::boolean(od.readOnly));
    if (od.lock) {
        Value lock = Value::object();
        lock.set("user", Value::string(od.lock->user));
        lock.set("host", Value::string(od.lock->host));
        lock.set("pid", Value::integer(od.lock->pid));
        lock.set("sinceUnixMs", Value::integer(od.lock->sinceUnixMs));
        lock.set("stale", Value::boolean(od.lock->stale));
        out.set("lock", std::move(lock));
    }
    if (od.document) {
        out.set("stateHash", Value::string(od.document->state()->stateHash().hex()));
        out.set("dirty", Value::boolean(od.document->dirty() || od.modified));
    }
    return out;
}

// UI thread hop from a long-running action: claims the document (busy) so it cannot close under the job.
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

Result<Value> describeOnUi(AppImpl& app, const std::string& id, JobContext& ctx)
{
    return app.bridge().call<Value>(
        [&app, id]() -> Result<Value> {
            OpenDocument* od = app.findDocument(id);
            if (!od) {
                return fail(ErrorCode::NotFound, "The document was closed");
            }
            return documentResult(*od);
        },
        ctx.cancel);
}

struct CommandText {
    const char* name;
    const char* title;
    const char* description;
    const char* category = "Document";
};
constexpr CommandText kCommandTexts[] = {
    {"feature.add", "Add Feature", "Adds a feature to the history"},
    {"feature.edit", "Edit Feature", "Changes a feature's name or properties"},
    {"feature.remove", "Remove Feature", "Removes a feature from the history"},
    {"feature.reorder", "Reorder Feature", "Moves a feature to another place in the history"},
    {"feature.suppress", "Suppress Feature", "Suppresses or unsuppresses a feature"},
    {"feature.rollback", "Move Rollback Bar",
     "Builds the history only up to a feature ('to'); new features go in after it. Without 'to', builds it all"},
    {"param.set", "Set Parameter", "Adds or changes a named parameter"},
    {"param.rename", "Rename Parameter", "Renames a parameter and every expression that uses it"},
    {"param.remove", "Delete Parameter", "Deletes a parameter that no expression uses"},
    {"body.set", "Edit Body", "Changes a body's name, role, colour or visibility"},
    {"document.import", "Add Import Feature",
     "Adds a prepared import feature and its bodies (file.import reads the file and does this)"},
    // The parametric features (T-096): props are checked against the type's schema; an edit replaces the props it
    // names (null removes an optional one).
    {"feature.datum_plane.add", "Datum Plane", "Adds a plane offset from a principal plane, a datum or a planar face",
     "Model"},
    {"feature.datum_plane.edit", "Edit Datum Plane", "Changes a datum plane's base, offset or direction", "Model"},
    {"feature.sketch.add", "New Sketch", "Adds a sketch on a principal plane, a datum plane or a planar face", "Model"},
    {"feature.sketch.edit", "Edit Sketch Feature", "Changes a sketch feature's plane or replaces its props", "Model"},
    {"feature.extrude.add", "Extrude", "Extrudes sketch regions into a new body, or joins, cuts or intersects a body",
     "Model"},
    {"feature.extrude.edit", "Edit Extrude", "Changes an extrude's distance, direction, draft, regions or operation",
     "Model"},
    {"feature.revolve.add", "Revolve",
     "Revolves sketch regions about a sketch line or a world axis into a new body, or joins, cuts or intersects one",
     "Model"},
    {"feature.revolve.edit", "Edit Revolve", "Changes a revolve's axis, angle, regions or operation", "Model"},
    {"feature.fillet.add", "Fillet", "Rounds edges of a body with a radius", "Model"},
    {"feature.fillet.edit", "Edit Fillet", "Changes a fillet's edges or radius", "Model"},
    {"feature.chamfer.add", "Chamfer", "Bevels edges of a body by a distance", "Model"},
    {"feature.chamfer.edit", "Edit Chamfer", "Changes a chamfer's edges or distance", "Model"},
    {"feature.hole.add", "Hole", "Drills simple, tapped, counterbored or countersunk holes at sketch points", "Model"},
    {"feature.hole.edit", "Edit Hole", "Changes a hole's kind, sizes, depth, thread or points", "Model"},
    // T-077: placing a body (a fixture against the part) by a face mate and offsets.
    {"feature.transform_body.add", "Move Body",
     "Moves a body: a face of it onto a face of another body (opposed, centred), then a gap, a turn and X, Y, Z "
     "offsets",
     "Model"},
    {"feature.transform_body.edit", "Edit Move Body", "Changes a body move's mate, gap, turn or offsets", "Model"},
    // T-082: the direct edits, for imported bodies as for modelled ones.
    {"feature.delete_face.add", "Delete Faces",
     "Removes faces of a body and heals the gap: a hole (by any face of it), a pocket, a fillet or a whole blend run",
     "Model"},
    {"feature.delete_face.edit", "Edit Delete Faces", "Changes the faces a face deletion removes", "Model"},
    {"feature.offset_face.add", "Push/Pull", "Moves planar faces of a body along their normals by a distance", "Model"},
    {"feature.offset_face.edit", "Edit Push/Pull", "Changes a push/pull's faces or distance", "Model"},
    {"feature.resize_hole.add", "Resize Hole", "Gives a hole a new diameter, by any face of its bore", "Model"},
    {"feature.resize_hole.edit", "Edit Resize Hole", "Changes a hole resize's hole or diameter", "Model"},
    {"sketch.set", "Replace Sketch", "Replaces the whole model of a sketch (and optionally its plane)", "Sketch"},
    {"sketch.entity.add", "Add Sketch Entities", "Adds points, lines, arcs, circles, splines or slots to a sketch",
     "Sketch"},
    {"sketch.entity.remove", "Delete Sketch Entities",
     "Deletes sketch entities, the curves on deleted points, unused end points and their constraints", "Sketch"},
    {"sketch.constraint.add", "Add Constraints", "Adds geometric constraints or dimensions to a sketch", "Sketch"},
    {"sketch.constraint.remove", "Delete Constraints", "Deletes constraints or dimensions of a sketch", "Sketch"},
    {"sketch.constraint.set", "Edit Dimension", "Changes a dimension's value or expression, or makes it driven",
     "Sketch"},
    {"sketch.split", "Split Curve", "Splits a sketch line or arc at a point; the first part keeps its id", "Sketch"},
    {"sketch.trim", "Trim Curve",
     "Removes the piece of a line, arc or circle between its nearest crossings; the rest keeps its id", "Sketch"},
    // T-092: the drawing tools' commands.
    {"sketch.construction", "Construction Geometry",
     "Marks sketch entities as construction geometry, or as profile geometry again", "Sketch"},
    {"sketch.extend", "Extend Curve", "Grows the end of a sketch line or arc to the next curve it meets", "Sketch"},
    {"sketch.offset", "Offset Curves",
     "Copies sketch lines, arcs and circles at a distance to one side; chains stay joined", "Sketch"},
    {"sketch.fillet", "Sketch Fillet", "Rounds the corner of two sketch lines with a tangent arc", "Sketch"},
    // T-093
    {"sketch.dimension.place", "Move Dimension Label", "Moves where a sketch dimension's value label is shown",
     "Sketch"},
    // T-032: setups, operations and the library items a document embeds.
    {"cam.setup.add", "New Setup", "Adds a setup: the part bodies, fixtures, stock, work coordinates and machine",
     "CAM"},
    {"cam.setup.edit", "Edit Setup", "Changes a setup's bodies, stock, work coordinates, machine, post or program",
     "CAM"},
    {"cam.setup.remove", "Delete Setup", "Deletes a setup and its operations", "CAM"},
    {"cam.setup.reorder", "Reorder Setup", "Moves a setup to another place in the program", "CAM"},
    {"cam.operation.add", "New Operation", "Adds a machining operation to a setup", "CAM"},
    {"cam.operation.edit", "Edit Operation",
     "Changes an operation's tool, cutting data, heights, geometry or parameters", "CAM"},
    {"cam.operation.remove", "Delete Operation", "Deletes an operation", "CAM"},
    {"cam.operation.reorder", "Reorder Operation", "Moves an operation within its setup or to another setup", "CAM"},
    {"cam.operation.suppress", "Suppress Operation", "Suppresses or unsuppresses an operation", "CAM"},
    {"cam.library.embed", "Embed Library Items",
     "Copies tools, holders, machines or materials into the document, or updates the copies it has", "CAM"},
    // T-057: several CAM commands as one undo step (applying a template set).
    {"cam.batch", "Several CAM Changes",
     "Applies several CAM commands (operations, library items, a setup edit, fixture moves) in order as one undo step",
     "CAM"},
};

std::string titleCase(std::string_view name)
{
    std::string out;
    bool start = true;
    for (const char c : name) {
        if (c == '.' || c == '_' || c == '-') {
            out += ' ';
            start = true;
        } else if (start && c >= 'a' && c <= 'z') {
            out += static_cast<char>(c - 'a' + 'A');
            start = false;
        } else {
            out += c;
            start = false;
        }
    }
    return out;
}

// The command's own schema with an optional "document" property added.
std::string commandSchema(std::string_view commandSchemaJson)
{
    auto parsed = json::parse(commandSchemaJson);
    if (!parsed || !parsed->isObject()) {
        return schemaOf(Value::object({{"document", documentProp()}}));
    }
    Value schema = std::move(*parsed);
    Value* properties = schema.find("properties");
    if (properties == nullptr || !properties->isObject()) {
        schema.set("properties", Value::object());
        properties = schema.find("properties");
    }
    properties->set("document", documentProp());
    // "minProperties" counted the command's own arguments; "document" must not satisfy it.
    if (Value* minimum = schema.find("minProperties"); minimum && minimum->isNumber()) {
        Value any = Value::array();
        for (const auto& [key, value] : properties->asObject()) {
            if (key != "document" && key != "id") {
                any.push(Value::object({{"required", Value::array({Value::string(key)})}}));
            }
        }
        schema.erase("minProperties");
        if (!any.asArray().empty()) {
            schema.set("anyOf", std::move(any));
        }
    }
    return json::canonical(schema);
}

// A document command, from the worker its action runs on (AppImpl::runOnWorker), so the UI thread never waits for
// what it computes (a sketch solve: PLAN.md 7). The registry starts it only after the document commands, undos and
// redos invoked before it have ended (ActionOrder::Ordered), so it applies on their result, as IDocument::execute on
// the UI thread did.
//   1. UI thread: the document is claimed, so it cannot close meanwhile.
//   2. Here: the command is applied to the document's state (Document::prepare; a solve stops when the action is
//      cancelled).
//   3. UI thread: it is committed with its journal record (AppImpl::commit). If the state changed in between (a change
//      made outside the ordered actions), it is prepared again on the new state; after three such misses it is
//      executed on the UI thread, so it always ends. The commit (or that execute) is the job's point of no return
//      (AppImpl::pointOfNoReturn): a job that reports Cancelled changed nothing, and one that changed the document
//      reports its result, whenever and from whichever thread it is cancelled.
// The arguments of a command checked against the snapshot it is applied to, and possibly completed (empty: as given).
using ArgsCheck = std::function<Result<doc::Command>(const doc::DocState&, const doc::Command&)>;

} // namespace

namespace {

// The heights an add or edit gives, over the operation's for an edit; nullopt when a member is missing or not a
// number (the command reports that itself).
std::optional<cam::Heights> givenHeights(const Value& args, const cam::Operation* current)
{
    cam::Heights h = current != nullptr ? current->heights : cam::Heights{};
    const Value* given = args.find("heights");
    if (given == nullptr) {
        return current != nullptr ? std::optional<cam::Heights>(h) : std::nullopt;
    }
    if (!given->isObject()) {
        return std::nullopt;
    }
    for (const auto& [key, slot] :
         {std::pair<const char*, double*>{"clearance", &h.clearance},
          std::pair<const char*, double*>{"retract", &h.retract},
          std::pair<const char*, double*>{"feedPlane", &h.feedPlane}, std::pair<const char*, double*>{"top", &h.top},
          std::pair<const char*, double*>{"bottom", &h.bottom}}) {
        const Value* v = given->find(key);
        if (v == nullptr && current == nullptr) {
            return std::nullopt;
        }
        if (v != nullptr) {
            if (!v->isNumber()) {
                return std::nullopt;
            }
            *slot = v->asNumber();
        }
    }
    return h;
}

// "from the stock top" for a reference of x-heightsUsed (docs/operation_types.md).
std::string fromWords(const std::string& from)
{
    if (from == "stockTop") {
        return "from the stock top";
    }
    if (from == "partTop") {
        return "from the part top";
    }
    if (from == "workZero") {
        return "from the work zero";
    }
    if (from == "eachPick") {
        return "from each pick";
    }
    if (from == "highestPick") {
        return "from the highest pick";
    }
    return "from " + from;
}

std::string mmWords(double v)
{
    std::string s = json::write(Value::number(v));
    return s + " mm";
}

} // namespace

// cam.operation.add and cam.operation.edit: "params" checked against the operation type's schema
// (IOperationType::paramsSchemaJson; the document has no operation registry, so its commands cannot), with the
// schema's top-level defaults filled in for params left out, so the document stores them; and "heights" against the
// order the schema's x-heightsUsed makes certain (cam_operation::heightsOutOfOrder: two heights the type reads from
// one reference; any other order is the type's own validation's). The type is "type", else (an edit) the operation's
// type in `state`. An edit is checked when it gives params or heights or changes the type (the operation's params are
// then checked, and completed, for the new type; its heights, with those given, for that type). A type the app has not
// registered is not checked: the engine reports such an operation Failed ("Unknown operation type"). Errors name the
// JSON Pointer of the bad value, "/params/...", or the height, "'heights.retract'".
Result<doc::Command> checkOperationParams(const cam::OperationRegistry* types, const doc::DocState& state,
                                          const doc::Command& command)
{
    auto args = json::parse(command.argsJson);
    if (types == nullptr || !args || !args->isObject()) {
        return command; // the command reports malformed arguments itself
    }
    const Value* typeArg = args->find("type");
    const cam::Operation* current = nullptr;
    bool checkParams = true, checkHeights = true;
    if (command.name == "cam.operation.edit") {
        const Value* id = args->find("id");
        if (id != nullptr && id->isString() && state.cam) {
            if (auto it = state.cam->operations.find(OperationId{Uuid::fromString(id->asString())});
                it != state.cam->operations.end()) {
                current = &it->second;
            }
        }
        if (current == nullptr) {
            return command; // the command reports the missing operation
        }
        const bool typeChanges = typeArg != nullptr && typeArg->isString() && typeArg->asString() != current->type;
        checkParams = args->find("params") != nullptr || typeChanges;
        checkHeights = args->find("heights") != nullptr || typeChanges;
        if (!checkParams && !checkHeights) {
            return command;
        }
    }
    const std::string type = typeArg != nullptr && typeArg->isString() ? typeArg->asString()
                             : current != nullptr                      ? current->type
                                                                       : std::string();
    const cam::IOperationType* opType = types->find(type);
    if (opType == nullptr) {
        return command;
    }
    auto schemaValue = json::parse(opType->paramsSchemaJson());
    if (!schemaValue) {
        return fail(ErrorCode::Internal, "The params schema of operation type '" + type +
                                             "' is not valid JSON: " + schemaValue.error().message);
    }
    if (checkHeights) {
        if (const auto heights = givenHeights(*args, current)) {
            if (const auto bad = cam_operation::heightsOutOfOrder(opType->paramsSchemaJson(), *heights)) {
                const auto value = [&](const std::string& key) {
                    return key == "clearance"   ? heights->clearance
                           : key == "retract"   ? heights->retract
                           : key == "feedPlane" ? heights->feedPlane
                           : key == "top"       ? heights->top
                                                : heights->bottom;
                };
                return fail(ErrorCode::InvalidArgument,
                            command.name + ": 'heights." + bad->higher + "' (" + mmWords(value(bad->higher)) + " " +
                                fromWords(bad->higherFrom) + ") must not be below " + bad->lower + " (" +
                                mmWords(value(bad->lower)) + " " + fromWords(bad->lowerFrom) +
                                ") for an operation of type '" + type + "'");
            }
        }
    }
    if (!checkParams) {
        return command;
    }
    Value params = Value::object();
    if (const Value* given = args->find("params")) {
        params = *given;
    } else if (current != nullptr && !current->paramsJson.empty()) {
        auto stored = json::parse(current->paramsJson);
        if (stored) {
            params = std::move(*stored);
        }
    }
    schema::fillDefaults(*schemaValue, params); // objects only
    if (auto st = schema::validate(*schemaValue, params); !st) {
        Error e = st.error();
        if (e.code == ErrorCode::InvalidArgument) {
            // validate() says "<pointer>: <what>" ("(the arguments)" for the whole value): the pointer from the
            // command's arguments instead.
            const std::string path = e.context.empty() ? std::string() : e.context.front();
            const std::string prefix = (path.empty() ? std::string("(the arguments)") : path) + ": ";
            const std::string what = e.message.rfind(prefix, 0) == 0 ? e.message.substr(prefix.size()) : e.message;
            const std::string pointer = "/params" + path;
            e.message = command.name + ": invalid params for operation type '" + type + "': " + pointer + ": " + what;
            e.context = {pointer};
        }
        return tl::unexpected<Error>(std::move(e));
    }
    args->set("params", std::move(params));
    return doc::Command{command.name, json::canonical(*args), command.label};
}

namespace {

Result<ActionResult> runCommand(AppImpl& app, const std::string& docId, const doc::Command& given, JobContext& ctx,
                                const ArgsCheck& check = {})
{
    auto guard = claimOnUi(app, docId, ctx);
    if (!guard) {
        return tl::unexpected<Error>(guard.error());
    }
    DocEntry& entry = *(*guard)->get();
    constexpr int kPrepares = 3;
    for (int attempt = 1;; ++attempt) {
        // od.document is set when the entry is made and stays while it is claimed; prepare() may run on any thread.
        // The check reads the snapshot the command is then prepared on (a change in between is a stale prepare: the
        // loop checks again).
        const doc::DocStatePtr now = entry.od.document->state();
        auto checked = check ? check(*now, given) : Result<doc::Command>(given);
        if (!checked) {
            return tl::unexpected<Error>(checked.error());
        }
        auto prepared = entry.od.document->prepare(*checked, ctx);
        if (!prepared) {
            return tl::unexpected<Error>(prepared.error());
        }
        const bool last = attempt >= kPrepares;
        if (check && prepared->before != now && !last) {
            continue; // prepared on another snapshot than the one checked
        }
        auto done = app.bridge().call<std::optional<Value>>(
            [&app, &entry, &prepared, &given, &check, &ctx, &now, last]() -> Result<std::optional<Value>> {
                const bool stale = entry.od.document->state() != prepared->before || (check && prepared->before != now);
                if (stale && !last) {
                    return std::optional<Value>(); // changed meanwhile: prepare again (still cancellable)
                }
                // Executed here on the state as it is now: checked against that state.
                auto again = stale && check ? check(*entry.od.document->state(), given) : Result<doc::Command>(given);
                if (!again) {
                    return tl::unexpected<Error>(again.error());
                }
                // The point of no return: cancelled before it, nothing changes and the job reports Cancelled; after it
                // a cancel is too late and the job reports the commit.
                if (!app.pointOfNoReturn(ctx.cancel)) {
                    return fail(ErrorCode::Cancelled, "Cancelled");
                }
                if (stale) {
                    auto executed = app.execute(entry, *again);
                    if (!executed) {
                        return tl::unexpected<Error>(executed.error());
                    }
                    return std::optional<Value>(documentResult(entry.od));
                }
                auto committed = app.commit(entry, std::move(*prepared));
                if (!committed) {
                    return tl::unexpected<Error>(committed.error());
                }
                return std::optional<Value>(documentResult(entry.od));
            },
            ctx.cancel);
        if (!done) {
            return tl::unexpected<Error>(done.error());
        }
        if (*done) {
            return result(std::move(**done));
        }
    }
}

Status addCommandActions(AppImpl& app)
{
    const doc::CommandRegistry& commands = app.commandTypes();
    for (const std::string& name : commands.names()) {
        const doc::ICommandHandler* handler = commands.find(name);
        if (handler == nullptr) {
            continue;
        }
        ActionInfo info;
        info.id = name;
        info.title = titleCase(name);
        info.category = "Document";
        info.description = "Runs the document command " + name;
        for (const CommandText& t : kCommandTexts) {
            if (name == t.name) {
                info.title = t.title;
                info.description = t.description;
                info.category = t.category;
            }
        }
        info.argsSchemaJson = commandSchema(handler->argsSchemaJson());
        info.changesDocument = true;
        info.changesToolpathsOrGcode = handler->changesToolpathsOrGcode();
        const std::string title = info.title;
        auto fn = [&app, name, title](std::string_view argsJson, Caller, JobContext& ctx) -> Result<ActionResult> {
            auto args = parseArgs(argsJson);
            if (!args) {
                return tl::unexpected<Error>(args.error());
            }
            const std::string docId = args->stringOr("document", "");
            args->erase("document");
            std::string label = title;
            if (const Value* n = args->find("name"); n && n->isString()) {
                label += " " + n->asString();
            }
            ArgsCheck check;
            if (name == "cam.operation.add" || name == "cam.operation.edit") {
                check = [&app](const doc::DocState& state, const doc::Command& command) {
                    return checkOperationParams(app.operationTypes(), state, command);
                };
            } else if (name == "cam.batch") { // T-057
                check = [&app](const doc::DocState& state, const doc::Command& command) {
                    return checkBatchParams(app.operationTypes(), state, command);
                };
            }
            return runCommand(app, docId, doc::Command{name, json::canonical(*args), label}, ctx, check);
        };
        app.runOnWorker(info.id);
        app.setOrder(info.id, ActionOrder::Ordered);
        if (auto st = app.actions().add(std::move(info), std::move(fn)); !st) {
            return st;
        }
    }
    return {};
}

std::optional<kernel::Heal> healOf(const Value& args)
{
    const std::string heal = args.stringOr("heal", "");
    if (heal == "none") {
        return kernel::Heal::None;
    }
    if (heal == "reader") {
        return kernel::Heal::Reader;
    }
    if (heal == "full") {
        return kernel::Heal::Full;
    }
    return std::nullopt;
}

Status addExport(AppImpl& app, const char* id, const char* title, const char* format, ExportFormat kind)
{
    ActionInfo info;
    info.id = id;
    info.title = title;
    info.category = "File";
    info.description = std::string("Writes the document's built bodies to a ") + format + " file";
    Value props = Value::object(
        {{"path", prop("string", "Output file (UTF-8)")},
         {"document", documentProp()},
         {"bodies", Value::object({{"type", Value::string("array")},
                                   {"items", Value::object({{"type", Value::string("string")},
                                                            {"pattern", Value::string(kUuidPattern)}})},
                                   {"description", Value::string("Body ids; all built bodies when omitted")}})}});
    props.find("path")->set("minLength", Value::integer(1));
    if (kind == ExportFormat::Stl) {
        Value tol = prop("number", "Chord tolerance in mm");
        tol.set("exclusiveMinimum", Value::integer(0));
        tol.set("default", Value::number(0.01));
        props.set("chordTolerance", std::move(tol));
    }
    info.argsSchemaJson = schemaOf(std::move(props), {"path"});
    info.longRunning = true;
    return app.actions().add(
        std::move(info), [&app, kind](std::string_view argsJson, Caller, JobContext& ctx) -> Result<ActionResult> {
            auto args = parseArgs(argsJson);
            if (!args) {
                return tl::unexpected<Error>(args.error());
            }
            auto guard = claimOnUi(app, args->stringOr("document", ""), ctx);
            if (!guard) {
                return tl::unexpected<Error>(guard.error());
            }
            std::optional<std::vector<BodyId>> bodies;
            if (const Value* list = args->find("bodies")) {
                bodies.emplace();
                for (const Value& b : list->asArray()) {
                    bodies->push_back(BodyId{Uuid::fromString(b.asString())});
                }
            }
            const std::string path = args->stringOr("path", "");
            auto st = app.exportWork(*(*guard)->get(), path, kind, args->numberOr("chordTolerance", 0.01),
                                     bodies ? &*bodies : nullptr, ctx);
            if (!st) {
                return tl::unexpected<Error>(st.error());
            }
            Value out =
                Value::object({{"path", Value::string(path)}, {"document", Value::string((*guard)->get()->od.id)}});
            return result(std::move(out));
        });
}

// The JSON form of a naming::TopoRef that the parametric feature props take (fillet and chamfer "edges", a sketch or
// datum plane's "face"): naming's own (naming.h toJson, the one implementation), parsed into an action result value.
Result<Value> topoRefJson(const naming::TopoRef& ref)
{
    return json::parse(naming::toJson(ref));
}

// The build the model actions read: the document's latest, or (args "before") the model as it was just before that
// feature, built on the calling worker. NotFound when there is none yet or no such feature.
Result<doc::BuildResultPtr> modelBuild(AppImpl& app, const OpenDocument& od, const Value& args, JobContext& ctx)
{
    doc::BuildResultPtr build;
    if (const std::string beforeId = args.stringOr("before", ""); !beforeId.empty()) {
        const doc::DocStatePtr state = od.document->state();
        const doc::DocStatePtr snapshot = feature_edit::stateBefore(*state, FeatureId{Uuid::fromString(beforeId)});
        if (!snapshot) {
            return fail(ErrorCode::NotFound, "The document has no feature " + beforeId);
        }
        auto built = feature_edit::buildSnapshot(app, od.id, snapshot, ctx);
        if (!built) {
            return tl::unexpected<Error>(built.error());
        }
        build = std::move(*built);
    } else {
        build = od.document->build();
    }
    if (!build) {
        return fail(ErrorCode::NotFound, "The document has not been built yet; try again when the rebuild ends");
    }
    return build;
}

// model.blendRun (T-082, for the direct-edit tools of T-083): the faces of the blend run a face belongs to
// (kernel::blendRun: the fillets joined to it by tangent blends), as face indices of the same build, so "select the
// whole blend run" can pick them and make a reference to each with model.reference. Sorted, with the face itself;
// just [index] for a fillet that touches no other blend; [] for a face that is not a fillet (or a corner patch between
// fillets). Same arguments as model.reference, for faces.
Status addBlendRunAction(AppImpl& app)
{
    Value index = prop("integer", "A face's index in the body's build (as a viewport pick gives it)");
    index.set("minimum", Value::integer(0));
    Value body = prop("string", "The body id");
    body.set("pattern", Value::string(kUuidPattern));
    Value before = prop("string", "A feature id: the face is one of the model as it was just before that feature, not "
                                  "of the latest build");
    before.set("pattern", Value::string(kUuidPattern));
    ActionInfo info{"model.blendRun",
                    "Blend Run",
                    "Model",
                    "Returns the faces of the blend run a fillet face belongs to (the fillets joined to it by tangent "
                    "blends), as face indices: what a delete_face feature needs to remove a fillet that joins others",
                    schemaOf(Value::object({{"body", std::move(body)},
                                            {"index", std::move(index)},
                                            {"before", std::move(before)},
                                            {"document", documentProp()}}),
                             {"body", "index"}),
                    "",
                    false,
                    false,
                    false};
    app.runOnWorker(info.id);
    return app.actions().add(
        std::move(info), [&app](std::string_view argsJson, Caller, JobContext& ctx) -> Result<ActionResult> {
            auto args = parseArgs(argsJson);
            if (!args) {
                return tl::unexpected<Error>(args.error());
            }
            auto guard = claimOnUi(app, args->stringOr("document", ""), ctx);
            if (!guard) {
                return tl::unexpected<Error>(guard.error());
            }
            auto build = modelBuild(app, (*guard)->get()->od, *args, ctx);
            if (!build) {
                return tl::unexpected<Error>(build.error());
            }
            const BodyId bodyId{Uuid::fromString(args->stringOr("body", ""))};
            const auto it = (*build)->bodies.find(bodyId);
            if (it == (*build)->bodies.end() || it->second.shape.isNull()) {
                return fail(ErrorCode::NotFound, "The build has no body " + bodyId.value.toString());
            }
            const double i = args->numberOr("index", -1);
            const std::uint32_t faces = it->second.shape.count(kernel::SubKind::Face);
            if (!(i >= 0) || i >= static_cast<double>(faces)) {
                return fail(ErrorCode::InvalidArgument, "The body has no face " +
                                                            std::to_string(static_cast<long long>(i)) + " (it has " +
                                                            std::to_string(faces) + ")");
            }
            Value run = Value::array();
            for (const kernel::SubId& face : kernel::blendRun(
                     it->second.shape, kernel::SubId{kernel::SubKind::Face, static_cast<std::uint32_t>(i)})) {
                run.push(Value::integer(static_cast<std::int64_t>(face.index)));
            }
            return result(Value::object({{"faces", std::move(run)}}));
        });
}

// model.reference: a persistent reference to an element of the latest build of a document (T-096), or of the model
// as it was just before a feature ("before": what the feature tools pick in while they edit that feature). It runs
// on a worker: the model before a feature is built there (from the rebuilder's cache, so it is quick).
Status addModelActions(AppImpl& app)
{
    Value kind =
        Value::object({{"enum", Value::array({Value::string("face"), Value::string("edge"), Value::string("vertex")})},
                       {"description", Value::string("The kind of element")}});
    Value index = prop("integer", "The element's index in the body's latest build (as a viewport pick gives it)");
    index.set("minimum", Value::integer(0));
    Value body = prop("string", "The body id");
    body.set("pattern", Value::string(kUuidPattern));
    Value before = prop("string", "A feature id: the element is one of the model as it was just before that feature "
                                  "(the rollback bar on the feature before it), not of the latest build");
    before.set("pattern", Value::string(kUuidPattern));
    ActionInfo info{"model.reference",
                    "Reference Element",
                    "Model",
                    "Returns a persistent reference (TopoRef JSON) to a face, edge or vertex of a built body, for "
                    "feature props such as fillet edges or a sketch plane on a face",
                    schemaOf(Value::object({{"body", std::move(body)},
                                            {"kind", std::move(kind)},
                                            {"index", std::move(index)},
                                            {"before", std::move(before)},
                                            {"document", documentProp()}}),
                             {"body", "kind", "index"}),
                    "",
                    false,
                    false,
                    false};
    app.runOnWorker(info.id);
    return app.actions().add(
        std::move(info), [&app](std::string_view argsJson, Caller, JobContext& ctx) -> Result<ActionResult> {
            auto args = parseArgs(argsJson);
            if (!args) {
                return tl::unexpected<Error>(args.error());
            }
            auto guard = claimOnUi(app, args->stringOr("document", ""), ctx);
            if (!guard) {
                return tl::unexpected<Error>(guard.error());
            }
            const OpenDocument& od = (*guard)->get()->od; // stays while claimed
            auto built = modelBuild(app, od, *args, ctx);
            if (!built) {
                return tl::unexpected<Error>(built.error());
            }
            const doc::BuildResultPtr build = std::move(*built);
            const BodyId bodyId{Uuid::fromString(args->stringOr("body", ""))};
            const auto it = build->bodies.find(bodyId);
            if (it == build->bodies.end() || it->second.shape.isNull()) {
                return fail(ErrorCode::NotFound, "The latest build has no body " + bodyId.value.toString());
            }
            const std::string kindName = args->stringOr("kind", "");
            const kernel::SubKind k = kindName == "face"   ? kernel::SubKind::Face
                                      : kindName == "edge" ? kernel::SubKind::Edge
                                                           : kernel::SubKind::Vertex;
            const double i = args->numberOr("index", -1);
            if (!(i >= 0) || i >= static_cast<double>(it->second.shape.count(k))) {
                return fail(ErrorCode::InvalidArgument, "The body has no " + kindName + " " +
                                                            std::to_string(static_cast<long long>(i)) + " (it has " +
                                                            std::to_string(it->second.shape.count(k)) + ")");
            }
            auto resolver = naming::makeResolver({naming::ResolverBody{bodyId, it->second.map, it->second.shape}});
            if (!resolver) {
                return fail(ErrorCode::Internal, "Out of memory");
            }
            const naming::TopoRef ref = resolver->makeRef(bodyId, kernel::SubId{k, static_cast<std::uint32_t>(i)});
            if (ref.names.empty()) {
                return fail(ErrorCode::NotFound, "That element has no persistent name in this build");
            }
            auto refJson = topoRefJson(ref);
            if (!refJson) {
                return fail(ErrorCode::Internal, "The reference could not be written: " + refJson.error().message);
            }
            return result(Value::object({{"ref", std::move(*refJson)}}));
        });
}

// standards.threads.list: the thread table of the hole feature (T-097).
Status addStandardsActions(AppImpl& app)
{
    Value series =
        Value::object({{"enum", Value::array({Value::string("iso_metric_coarse"), Value::string("iso_metric_fine"),
                                              Value::string("unc"), Value::string("unf")})},
                       {"description", Value::string("One series; all when omitted")}});
    ActionInfo info{"standards.threads.list",
                    "Thread Sizes",
                    "Model",
                    "Lists the thread series (ISO metric coarse and fine, UNC, UNF) with major diameter, pitch or "
                    "threads per inch and tap drill of each size, for the hole feature's thread",
                    schemaOf(Value::object({{"series", std::move(series)}})),
                    "",
                    false,
                    false,
                    false};
    return app.actions().add(
        std::move(info), [](std::string_view argsJson, Caller, JobContext&) -> Result<ActionResult> {
            auto args = parseArgs(argsJson);
            if (!args) {
                return tl::unexpected<Error>(args.error());
            }
            auto list = doc::features::threadListJson(args->stringOr("series", ""));
            if (!list) {
                return tl::unexpected<Error>(list.error());
            }
            auto parsed = json::parse(*list);
            if (!parsed) {
                return fail(ErrorCode::Internal, "The thread table could not be read: " + parsed.error().message);
            }
            return result(std::move(*parsed));
        });
}

} // namespace

Status registerBuiltinActions(AppImpl& app)
{
    IActionRegistry& actions = app.actions();
    const auto add = [&actions](ActionInfo info, ActionFn fn) {
        return actions.add(std::move(info), std::move(fn));
    };
    Status st;

    // ---- app ----
    st = add(ActionInfo{"app.about", "About RayCAD", "App", "Name, version and licence of this build",
                        schemaOf(Value::object()), "", false, false, false},
             [](std::string_view, Caller, JobContext&) -> Result<ActionResult> {
                 return result(Value::object({{"name", Value::string("RayCAD")},
                                              {"version", Value::string(RAY_VERSION_STRING)},
                                              {"license", Value::string("LGPL-2.1-or-later")}}));
             });
    if (!st) {
        return st;
    }
    st = add(ActionInfo{"app.version", "Version", "App", "The version of this build", schemaOf(Value::object()), "",
                        false, false, false},
             [](std::string_view, Caller, JobContext&) -> Result<ActionResult> {
                 return result(Value::object({{"version", Value::string(RAY_VERSION_STRING)}}));
             });
    if (!st) {
        return st;
    }

    // ---- file ----
    {
        Value tpl = prop("string", "Template id; \"\" is the default (inch)");
        tpl.set("default", Value::string(""));
        st = add(ActionInfo{"file.new", "New Document", "File", "Creates an empty document",
                            schemaOf(Value::object({{"template", std::move(tpl)}})), "Ctrl+N", false, false, false},
                 [&app](std::string_view argsJson, Caller, JobContext&) -> Result<ActionResult> {
                     auto args = parseArgs(argsJson);
                     if (!args) {
                         return tl::unexpected<Error>(args.error());
                     }
                     auto od = app.newDocument(args->stringOr("template", ""));
                     if (!od) {
                         return tl::unexpected<Error>(od.error());
                     }
                     return result(documentResult(**od));
                 });
        if (!st) {
            return st;
        }
    }
    {
        Value path = prop("string", ".raycad document, or a STEP or IGES file to import into a new document");
        path.set("minLength", Value::integer(1));
        st = add(ActionInfo{"file.open", "Open...", "File", "Opens a document, or imports a STEP or IGES file",
                            schemaOf(Value::object({{"path", std::move(path)}}), {"path"}), "Ctrl+O", false, false,
                            true},
                 [&app](std::string_view argsJson, Caller, JobContext& ctx) -> Result<ActionResult> {
                     auto args = parseArgs(argsJson);
                     if (!args) {
                         return tl::unexpected<Error>(args.error());
                     }
                     auto od = app.openWork(args->stringOr("path", ""), ctx);
                     if (!od) {
                         return tl::unexpected<Error>(od.error());
                     }
                     auto described = describeOnUi(app, (*od)->id, ctx);
                     if (!described) {
                         return tl::unexpected<Error>(described.error());
                     }
                     return result(std::move(*described));
                 });
        if (!st) {
            return st;
        }
    }
    const auto saveFn = [&app](bool as) {
        return [&app, as](std::string_view argsJson, Caller, JobContext& ctx) -> Result<ActionResult> {
            auto args = parseArgs(argsJson);
            if (!args) {
                return tl::unexpected<Error>(args.error());
            }
            auto guard = claimOnUi(app, args->stringOr("document", ""), ctx);
            if (!guard) {
                return tl::unexpected<Error>(guard.error());
            }
            DocEntry& entry = *(*guard)->get();
            std::optional<std::string> path;
            if (as) {
                path = args->stringOr("path", "");
            }
            auto commit = app.saveWork(entry, path, args->boolOr("overwrite", false), ctx);
            if (!commit) {
                return tl::unexpected<Error>(commit.error());
            }
            auto described = describeOnUi(app, entry.od.id, ctx);
            if (!described) {
                return tl::unexpected<Error>(described.error());
            }
            described->set("commit", Value::string(commit->id.hex()));
            return result(std::move(*described));
        };
    };
    st = add(ActionInfo{"file.save", "Save", "File", "Saves the document (a new version in its .raycad file)",
                        schemaOf(Value::object({{"document", documentProp()}})), "Ctrl+S", false, false, true},
             saveFn(false));
    if (!st) {
        return st;
    }
    {
        Value path = prop("string", "The new .raycad file (UTF-8)");
        path.set("minLength", Value::integer(1));
        Value overwrite = prop("boolean", "Replace an existing RayCAD file that nobody has open");
        overwrite.set("default", Value::boolean(false));
        st = add(ActionInfo{"file.saveAs", "Save As...", "File", "Saves the document as a new .raycad file",
                            schemaOf(Value::object({{"path", std::move(path)},
                                                    {"overwrite", std::move(overwrite)},
                                                    {"document", documentProp()}}),
                                     {"path"}),
                            "Ctrl+Shift+S", false, false, true},
                 saveFn(true));
        if (!st) {
            return st;
        }
    }
    {
        Value discard = prop("boolean", "Close even with unsaved changes");
        discard.set("default", Value::boolean(false));
        st = add(
            ActionInfo{"file.close", "Close", "File", "Closes the document",
                       schemaOf(Value::object({{"document", documentProp()}, {"discardChanges", std::move(discard)}})),
                       "Ctrl+W", false, false, false},
            [&app](std::string_view argsJson, Caller, JobContext&) -> Result<ActionResult> {
                auto args = parseArgs(argsJson);
                if (!args) {
                    return tl::unexpected<Error>(args.error());
                }
                auto entry = app.resolve(args->stringOr("document", ""));
                if (!entry) {
                    return tl::unexpected<Error>(entry.error());
                }
                OpenDocument& od = (*entry)->od;
                if ((od.document->dirty() || od.modified) && !args->boolOr("discardChanges", false)) {
                    return fail(ErrorCode::InvalidArgument,
                                "'" + od.title + "' has unsaved changes; save it, or pass discardChanges");
                }
                const std::string id = od.id;
                if (auto closed = app.close(od); !closed) {
                    return tl::unexpected<Error>(closed.error());
                }
                return result(Value::object({{"closed", Value::string(id)}}));
            });
        if (!st) {
            return st;
        }
    }
    {
        Value path = prop("string", "STEP or IGES file (UTF-8)");
        path.set("minLength", Value::integer(1));
        Value heal = Value::object(
            {{"enum", Value::array({Value::string("none"), Value::string("reader"), Value::string("full")})},
             {"description", Value::string("Healing; full when omitted")}});
        Value role = Value::object({{"enum", Value::array({Value::string("part"), Value::string("fixture")})},
                                    {"default", Value::string("part")},
                                    {"description", Value::string("Fixture bodies are static collision bodies")}});
        Value name = prop("string", "Feature name; \"Import <file name>\" when omitted");
        name.set("minLength", Value::integer(1));
        st = add(ActionInfo{"file.import", "Import...", "File",
                            "Imports a STEP or IGES file into the document as one import feature (a new document when "
                            "none is open)",
                            schemaOf(Value::object({{"path", std::move(path)},
                                                    {"heal", std::move(heal)},
                                                    {"role", std::move(role)},
                                                    {"name", std::move(name)},
                                                    {"document", documentProp()}}),
                                     {"path"}),
                            "Ctrl+I", true, false, true},
                 [&app](std::string_view argsJson, Caller, JobContext& ctx) -> Result<ActionResult> {
                     auto args = parseArgs(argsJson);
                     if (!args) {
                         return tl::unexpected<Error>(args.error());
                     }
                     AppImpl::ImportOptions options;
                     options.heal = healOf(*args);
                     options.role =
                         args->stringOr("role", "part") == "fixture" ? doc::Body::Role::Fixture : doc::Body::Role::Part;
                     options.name = args->stringOr("name", "");
                     auto id = app.importWork(args->stringOr("document", ""), args->stringOr("path", ""), options, ctx);
                     if (!id) {
                         return tl::unexpected<Error>(id.error());
                     }
                     auto described = describeOnUi(app, *id, ctx);
                     if (!described) {
                         return tl::unexpected<Error>(described.error());
                     }
                     return result(std::move(*described));
                 });
        if (!st) {
            return st;
        }
    }
    if (st = addExport(app, "file.exportStep", "Export STEP...", "STEP (AP214)", ExportFormat::Step); !st) {
        return st;
    }
    if (st = addExport(app, "file.exportIges", "Export IGES...", "IGES", ExportFormat::Iges); !st) {
        return st;
    }
    if (st = addExport(app, "file.exportStl", "Export STL...", "STL", ExportFormat::Stl); !st) {
        return st;
    }

    // ---- edit ----
    // Undo and redo wait for the document commands invoked before them (and the commands after them wait in turn);
    // save and close wait for those before them, so they see their edits (ActionOrder).
    app.setOrder("edit.undo", ActionOrder::Ordered);
    app.setOrder("edit.redo", ActionOrder::Ordered);
    app.setOrder("file.save", ActionOrder::After);
    app.setOrder("file.saveAs", ActionOrder::After);
    app.setOrder("file.close", ActionOrder::After);
    for (const bool redo : {false, true}) {
        st = add(ActionInfo{redo ? "edit.redo" : "edit.undo", redo ? "Redo" : "Undo", "Edit",
                            redo ? "Repeats the last undone change" : "Reverts the last change",
                            schemaOf(Value::object({{"document", documentProp()}})), redo ? "Ctrl+Y" : "Ctrl+Z", true,
                            false, false},
                 [&app, redo](std::string_view argsJson, Caller, JobContext&) -> Result<ActionResult> {
                     auto args = parseArgs(argsJson);
                     if (!args) {
                         return tl::unexpected<Error>(args.error());
                     }
                     auto entry = app.resolve(args->stringOr("document", ""));
                     if (!entry) {
                         return tl::unexpected<Error>(entry.error());
                     }
                     if (auto done = app.undo(**entry, redo); !done) {
                         return tl::unexpected<Error>(done.error());
                     }
                     return result(documentResult((*entry)->od));
                 });
        if (!st) {
            return st;
        }
    }

    // ---- measure (T-022) ----
    if (st = registerMeasureActions(app); !st) {
        return st;
    }

    // ---- posting (T-047) ----
    if (st = registerCamPostActions(app); !st) {
        return st;
    }

    // ---- operation templates (T-057) ----
    if (st = registerCamTemplateActions(app); !st) {
        return st;
    }

    // ---- model references (T-096) ----
    if (st = addModelActions(app); !st) {
        return st;
    }
    if (st = addBlendRunAction(app); !st) { // T-082
        return st;
    }

    // ---- standards tables (T-097) ----
    if (st = addStandardsActions(app); !st) {
        return st;
    }

    // ---- every document command ----
    return addCommandActions(app);
}

} // namespace ray::api::detail
