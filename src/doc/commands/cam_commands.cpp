// The CAM commands (task T-032): setups, operations and embedded library items, as document commands, so they are
// saved, journalled, undone and redone with everything else. Each is a pure function of (snapshot, canonical
// arguments); the state after an edit is a new CamState (the old snapshot keeps its own).
//
// Arguments (JSON objects; any other key is refused; ids are lower-case UUIDs, never the nil one; the JSON forms of
// stock, wcs, tool, cutting, heights, geometry and library items are those of cam_json.h):
//   cam.setup.add        {"id", "name", "partBodies": [body...], "fixtureBodies"?: [], "stock"?: Stock (a box with no
//                        offsets), "wcs"?: Wcs (corner 0 of the stock, G54), "machine"?: uuid | null, "post"?: (the
//                        machine's default post), "programNumber"?: 1001, "programComment"?: "", "index"?: <end>,
//                        "library"?: Library}
//   cam.setup.edit       {"id", any of the members above but index}: the given members are replaced whole
//   cam.setup.remove     {"id"}               removes the setup and its operations
//   cam.setup.reorder    {"id", "index"}      moves the setup to `index` of the resulting list
//   cam.operation.add    {"id", "setup", "type", "name", "tool": ToolRef, "cutting": Cutting, "heights": Heights,
//                        "geometry"?: [], "params"?: {}, "template"?: "", "suppressed"?: false, "index"?: <end>,
//                        "library"?: Library}
//                        ToolRef defaults: number = the tool's defaultToolNumber (else 1), lengthOffset and
//                        diameterOffset = number, stickOut = the tool's shoulder length (else its flute length),
//                        holder = none. Cutting defaults: feedRamp, feedLeadIn, feedLeadOut = feedCut, clockwise,
//                        coolant off.
//   cam.operation.edit   {"id", "type"?, "name"?, "tool"?, "cutting"?, "heights"?, "geometry"?, "params"?, "template"?,
//                        "library"?}: tool, cutting and heights change the members given; geometry and params are
//                        replaced whole
//   cam.operation.remove {"id"}
//   cam.operation.reorder {"id", "index", "setup"?}  moves the operation to `index` of its setup's list (or of
//                        `setup`'s, moving it there)
//   cam.operation.suppress {"id", "suppressed"}
//   cam.library.embed    {"tools"?, "holders"?, "machines"?, "materials"?}: embeds the items, or replaces the embedded
//                        copy of the same id (an updated library item: the operations using it go stale)
// Library = {"tools"?: [Tool], "holders"?: [Holder], "machines"?: [Machine], "materials"?: [Material]}: library items
// embedded in the same step (PLAN.md 3.1: a document carries a copy of every library item it uses, taken on first use).
// Checks: bodies are bodies of the document (part and fixture bodies distinct, at least one part); a "fromSetup" stock
// names an earlier setup; the machine, tool and holder named are embedded (or come in "library"); an operation's
// geometry lies on its setup's part bodies; heights are ordered (cam_json.h). Params are not checked against the
// operation type's schema here (the document has no operation registry: the engine and the API do that).
// Every command here changes toolpaths or G-code (changesToolpathsOrGcode() is true), so automation asks the user.
// A state whose CAM state becomes empty stores none (DocState::cam null), so it hashes like one that never had any.
#include "args.h"
#include "cam_json.h"
#include "commands.h"

#include "../features/feature_props.h"

#include <ray/cam.h>

#include <algorithm>
#include <exception>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ray::doc {

// ---------- The CAM engine's view of a snapshot (document.h) ----------

cam::CamSnapshot camSnapshot(const DocStatePtr& state, const BuildResultPtr& build)
{
    cam::CamSnapshot out;
    if (state) {
        out.cam = state->cam;
    }
    if (build) {
        out.built = true;
        out.bodies.reserve(build->bodies.size());
        for (const auto& [id, body] : build->bodies) { // std::map: sorted by id
            out.bodies.push_back(naming::ResolverBody{id, body.map, body.shape});
        }
    }
    return out;
}

namespace commands {
namespace {

using camjson::Source;
using json::Value;

#define RAY_CAM_TRY(var, expr)                                                                                         \
    auto var = (expr);                                                                                                 \
    if (!var) {                                                                                                        \
        return tl::unexpected(var.error());                                                                            \
    }
#define RAY_CAM_CHECK(expr)                                                                                            \
    do {                                                                                                               \
        if (auto ray_cam_status_ = (expr); !ray_cam_status_) {                                                         \
            return tl::unexpected(ray_cam_status_.error());                                                            \
        }                                                                                                              \
    } while (false)

using ApplyFn = Result<DocStatePtr> (*)(const DocState& before, std::string_view argsJson);

class CamCommand final : public ICommandHandler {
public:
    CamCommand(std::string_view name, std::string schema, ApplyFn fn) : name_(name), schema_(std::move(schema)), fn_(fn)
    {
    }
    std::string_view name() const override { return name_; }
    std::string_view argsSchemaJson() const override { return schema_; }
    bool changesToolpathsOrGcode() const override { return true; }
    Result<DocStatePtr> apply(DocStatePtr before, std::string_view argsJson) const override
    {
        if (!before) {
            return invalidArgument(std::string(name_) + ": there is no document snapshot to apply it to");
        }
        try {
            return fn_(*before, argsJson);
        } catch (const std::bad_alloc&) {
            return tl::unexpected(Error{ErrorCode::Internal, std::string(name_) + ": out of memory", {}});
        } catch (...) {
            return tl::unexpected(Error{ErrorCode::Internal, std::string(name_) + ": unexpected failure", {}});
        }
    }

private:
    std::string_view name_;
    std::string schema_;
    ApplyFn fn_;
};

constexpr std::string_view kSetupAdd = "cam.setup.add";
constexpr std::string_view kSetupEdit = "cam.setup.edit";
constexpr std::string_view kSetupRemove = "cam.setup.remove";
constexpr std::string_view kSetupReorder = "cam.setup.reorder";
constexpr std::string_view kOpAdd = "cam.operation.add";
constexpr std::string_view kOpEdit = "cam.operation.edit";
constexpr std::string_view kOpRemove = "cam.operation.remove";
constexpr std::string_view kOpReorder = "cam.operation.reorder";
constexpr std::string_view kOpSuppress = "cam.operation.suppress";
constexpr std::string_view kLibraryEmbed = "cam.library.embed";

Source argsSource(const Args& args)
{
    return Source{Source::Kind::Args, args.command()};
}

// The state being edited: a copy of the snapshot with its own copy of the CAM state.
struct Edit {
    std::shared_ptr<DocState> doc;
    std::shared_ptr<cam::CamState> cam;
    DocStatePtr finish()
    {
        doc->cam = cam->empty() ? nullptr : std::shared_ptr<const cam::CamState>(cam);
        return doc;
    }
};

Edit edit(const DocState& before)
{
    Edit e;
    e.doc = std::make_shared<DocState>(before);
    e.cam = before.cam ? std::make_shared<cam::CamState>(*before.cam) : std::make_shared<cam::CamState>();
    return e;
}

std::ptrdiff_t setupIndex(const cam::CamState& cam, const SetupId& id)
{
    for (std::size_t i = 0; i < cam.setups.size(); ++i) {
        if (cam.setups[i].id == id) {
            return static_cast<std::ptrdiff_t>(i);
        }
    }
    return -1;
}

// The setup listing `op`, and the index there; -1 when none.
std::ptrdiff_t setupOfOp(const cam::CamState& cam, const OperationId& op, std::size_t* at = nullptr)
{
    for (std::size_t i = 0; i < cam.setups.size(); ++i) {
        const auto& ops = cam.setups[i].operations;
        const auto it = std::find(ops.begin(), ops.end(), op);
        if (it != ops.end()) {
            if (at != nullptr) {
                *at = static_cast<std::size_t>(it - ops.begin());
            }
            return static_cast<std::ptrdiff_t>(i);
        }
    }
    return -1;
}

Result<std::size_t> existingSetup(const Args& args, const cam::CamState& cam, std::string_view key = "id")
{
    RAY_CAM_TRY(id, args.id(key));
    const std::ptrdiff_t index = setupIndex(cam, SetupId{*id});
    if (index < 0) {
        return args.error(key, "names no setup of this document (" + id->toString() + ")");
    }
    return static_cast<std::size_t>(index);
}

Result<OperationId> existingOperation(const Args& args, const cam::CamState& cam)
{
    RAY_CAM_TRY(id, args.id("id"));
    if (cam.operations.count(OperationId{*id}) == 0) {
        return args.error("id", "names no operation of this document (" + id->toString() + ")");
    }
    return OperationId{*id};
}

bool validTypeName(std::string_view type)
{
    if (type.empty() || type.size() > 64) {
        return false;
    }
    return std::all_of(type.begin(), type.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
               c == '-';
    });
}

// "library": embeds the items it lists into `cam` (replacing a copy of the same id).
Status embedLibrary(const Args& args, const Value& library, cam::CamState& cam, std::string_view field)
{
    const Source source = argsSource(args);
    if (!library.isObject()) {
        return args.error(field, "must be a JSON object");
    }
    for (const json::Member& m : library.asObject()) {
        if (m.key != "tools" && m.key != "holders" && m.key != "machines" && m.key != "materials") {
            return args.error(std::string(field) + "." + m.key, "is not a kind of library item");
        }
        if (!m.value.isArray()) {
            return args.error(std::string(field) + "." + m.key, "must be an array");
        }
    }
    const auto each = [&](const char* kind, auto read, auto& into) -> Status {
        const Value* items = library.find(kind);
        if (items == nullptr) {
            return {};
        }
        for (std::size_t i = 0; i < items->asArray().size(); ++i) {
            const std::string where =
                (field.empty() ? std::string() : std::string(field) + ".") + kind + "[" + std::to_string(i) + "]";
            auto item = read(items->asArray()[i], source, where);
            if (!item) {
                return tl::unexpected(item.error());
            }
            const auto id = item->item.id;
            into[id] = std::move(*item);
        }
        return {};
    };
    RAY_CAM_CHECK(each("tools", camjson::readTool, cam.tools));
    RAY_CAM_CHECK(each("holders", camjson::readHolder, cam.holders));
    RAY_CAM_CHECK(each("machines", camjson::readMachine, cam.machines));
    RAY_CAM_CHECK(each("materials", camjson::readMaterial, cam.materials));
    return {};
}

Status embedFromArgs(const Args& args, cam::CamState& cam)
{
    RAY_CAM_TRY(library, args.optValue("library"));
    if (*library != nullptr) {
        return embedLibrary(args, **library, cam, "library");
    }
    return {};
}

bool hasBody(const DocState& doc, const BodyId& id)
{
    return std::any_of(doc.bodies.begin(), doc.bodies.end(), [&](const Body& b) { return b.id == id; });
}

// The checks every setup must pass after an edit (index: where it is in cam.setups).
Status checkSetup(const Args& args, const DocState& doc, const cam::CamState& cam, std::size_t index)
{
    const cam::Setup& s = cam.setups[index];
    if (s.partBodies.empty()) {
        return args.error("partBodies", "must list at least one body");
    }
    for (const auto& [key, list] :
         {std::pair<const char*, const std::vector<BodyId>*>{"partBodies", &s.partBodies},
          std::pair<const char*, const std::vector<BodyId>*>{"fixtureBodies", &s.fixtureBodies}}) {
        for (const BodyId& b : *list) {
            if (!hasBody(doc, b)) {
                return args.error(key, "names body " + b.value.toString() + ", which is not in the document");
            }
        }
    }
    for (const BodyId& b : s.fixtureBodies) {
        if (std::find(s.partBodies.begin(), s.partBodies.end(), b) != s.partBodies.end()) {
            return args.error("fixtureBodies", "lists body " + b.value.toString() + ", which is a part body");
        }
    }
    if (const auto* solid = std::get_if<cam::SolidStock>(&s.stock); solid && !hasBody(doc, solid->body)) {
        return args.error("stock.body",
                          "names body " + solid->body.value.toString() + ", which is not in the document");
    }
    if (const auto* from = std::get_if<cam::FromSetupStock>(&s.stock)) {
        const std::ptrdiff_t previous = setupIndex(cam, from->previous);
        if (previous < 0 || static_cast<std::size_t>(previous) >= index) {
            return args.error("stock.setup", "must name an earlier setup");
        }
    }
    for (const auto& [key, ref] :
         {std::pair<const char*, const std::optional<naming::TopoRef>*>{"wcs.zFace", &s.wcs.zFace},
          std::pair<const char*, const std::optional<naming::TopoRef>*>{"wcs.xEdge", &s.wcs.xEdge},
          std::pair<const char*, const std::optional<naming::TopoRef>*>{"wcs.originVertex", &s.wcs.originVertex}}) {
        if (*ref && !hasBody(doc, (*ref)->body)) {
            return args.error(key, "is on body " + (*ref)->body.value.toString() + ", which is not in the document");
        }
    }
    if (s.machine.value != Uuid{} && cam.machines.count(s.machine) == 0) {
        return args.error("machine", "names machine " + s.machine.value.toString() +
                                         ", which is not embedded: give it in 'library'");
    }
    for (const OperationId& opId : s.operations) {
        const cam::Operation& op = cam.operations.at(opId);
        for (std::size_t i = 0; i < op.geometry.size(); ++i) {
            if (std::find(s.partBodies.begin(), s.partBodies.end(), op.geometry[i].body) == s.partBodies.end()) {
                return args.error("partBodies", "must keep body " + op.geometry[i].body.value.toString() +
                                                    ", which operation '" + op.name + "' machines");
            }
        }
    }
    return {};
}

// Every later setup whose stock comes from setup `index` still finds it earlier in the list.
Status checkStockChain(const Args& args, const cam::CamState& cam)
{
    for (std::size_t i = 0; i < cam.setups.size(); ++i) {
        if (const auto* from = std::get_if<cam::FromSetupStock>(&cam.setups[i].stock)) {
            const std::ptrdiff_t previous = setupIndex(cam, from->previous);
            if (previous < 0 || static_cast<std::size_t>(previous) >= i) {
                return args.error("the stock of setup '" + cam.setups[i].name +
                                  "' comes from a setup that would not come before it");
            }
        }
    }
    return {};
}

Status checkOperation(const Args& args, const cam::CamState& cam, const cam::Operation& op, const cam::Setup& setup)
{
    if (cam.tools.count(op.tool.tool) == 0) {
        return args.error("tool.tool", "names tool " + op.tool.tool.value.toString() +
                                           ", which is not embedded: give it in 'library'");
    }
    if (op.tool.holder.value != Uuid{} && cam.holders.count(op.tool.holder) == 0) {
        return args.error("tool.holder", "names holder " + op.tool.holder.value.toString() +
                                             ", which is not embedded: give it in 'library'");
    }
    for (std::size_t i = 0; i < op.geometry.size(); ++i) {
        if (std::find(setup.partBodies.begin(), setup.partBodies.end(), op.geometry[i].body) ==
            setup.partBodies.end()) {
            return args.error("geometry[" + std::to_string(i) + "]",
                              "is on body " + op.geometry[i].body.value.toString() +
                                  ", which is not a part body of setup '" + setup.name + "'");
        }
    }
    return {};
}

// The setup members of add and edit, read onto `s`.
Status readSetupMembers(const Args& args, cam::Setup& s, bool adding, const cam::CamState& cam)
{
    const Source source = argsSource(args);
    RAY_CAM_TRY(name, adding ? args.text("name").map([](std::string v) { return std::optional<std::string>(v); })
                             : args.optText("name"));
    if (*name) {
        s.name = std::move(**name);
    }
    for (const auto& [key, list] : {std::pair<const char*, std::vector<BodyId>*>{"partBodies", &s.partBodies},
                                    std::pair<const char*, std::vector<BodyId>*>{"fixtureBodies", &s.fixtureBodies}}) {
        RAY_CAM_TRY(value, args.optValue(key));
        if (*value != nullptr) {
            RAY_CAM_TRY(bodies, camjson::readBodies(**value, source, key));
            *list = std::move(*bodies);
        } else if (adding && std::string_view(key) == "partBodies") {
            return args.error(key, "is required");
        }
    }
    RAY_CAM_TRY(stock, args.optValue("stock"));
    if (*stock != nullptr) {
        RAY_CAM_TRY(read, camjson::readStock(**stock, source, "stock"));
        s.stock = std::move(*read);
    }
    RAY_CAM_TRY(wcs, args.optValue("wcs"));
    if (*wcs != nullptr) {
        RAY_CAM_TRY(read, camjson::readWcs(**wcs, source, "wcs"));
        s.wcs = std::move(*read);
    }
    RAY_CAM_TRY(machine, args.optValue("machine"));
    if (*machine != nullptr) {
        if ((*machine)->isNull()) {
            s.machine = MachineId{};
        } else {
            RAY_CAM_TRY(id, args.id("machine"));
            s.machine = MachineId{*id};
        }
    }
    RAY_CAM_TRY(post, args.optString("post"));
    if (*post) {
        s.postId = std::move(**post);
    } else if (adding) {
        const auto m = cam.machines.find(s.machine);
        s.postId = m != cam.machines.end() ? m->second.item.defaultPost : std::string();
    }
    RAY_CAM_TRY(number, args.optValue("programNumber"));
    if (*number != nullptr) {
        const Value& v = **number;
        if (!v.isNumber() || !(v.asNumber() >= 0 && v.asNumber() <= 99999999) ||
            static_cast<double>(static_cast<int>(v.asNumber())) != v.asNumber()) {
            return args.error("programNumber", "must be a whole number from 0 to 99999999");
        }
        s.programNumber = static_cast<int>(v.asNumber());
    }
    RAY_CAM_TRY(comment, args.optString("programComment"));
    if (*comment) {
        s.programComment = std::move(**comment);
    }
    return {};
}

// ---------- cam.setup.* ----------

Result<DocStatePtr> setupAdd(const DocState& before, std::string_view argsJson)
{
    RAY_CAM_TRY(args, Args::parse(kSetupAdd, argsJson,
                                  {"fixtureBodies", "id", "index", "library", "machine", "name", "partBodies", "post",
                                   "programComment", "programNumber", "stock", "wcs"}));
    RAY_CAM_TRY(id, args->id("id"));
    Edit e = edit(before);
    if (setupIndex(*e.cam, SetupId{*id}) >= 0) {
        return args->error("id", "is already used by a setup (" + id->toString() + ")");
    }
    RAY_CAM_TRY(index, args->optIndex("index", e.cam->setups.size()));
    RAY_CAM_CHECK(embedFromArgs(*args, *e.cam));
    cam::Setup s;
    s.id = SetupId{*id};
    s.stock = cam::BoxStock{0, 0, 0, 0, 0, 0};
    s.wcs.originMode = cam::Wcs::OriginMode::StockCorner;
    RAY_CAM_CHECK(readSetupMembers(*args, s, true, *e.cam));
    const std::size_t at = index->value_or(e.cam->setups.size());
    e.cam->setups.insert(e.cam->setups.begin() + static_cast<std::ptrdiff_t>(at), std::move(s));
    RAY_CAM_CHECK(checkSetup(*args, *e.doc, *e.cam, at));
    RAY_CAM_CHECK(checkStockChain(*args, *e.cam));
    return e.finish();
}

Result<DocStatePtr> setupEdit(const DocState& before, std::string_view argsJson)
{
    RAY_CAM_TRY(args, Args::parse(kSetupEdit, argsJson,
                                  {"fixtureBodies", "id", "library", "machine", "name", "partBodies", "post",
                                   "programComment", "programNumber", "stock", "wcs"}));
    Edit e = edit(before);
    RAY_CAM_TRY(index, existingSetup(*args, *e.cam));
    if (!args->has("fixtureBodies") && !args->has("library") && !args->has("machine") && !args->has("name") &&
        !args->has("partBodies") && !args->has("post") && !args->has("programComment") && !args->has("programNumber") &&
        !args->has("stock") && !args->has("wcs")) {
        return args->error("nothing to change: give at least one member of the setup");
    }
    RAY_CAM_CHECK(embedFromArgs(*args, *e.cam));
    RAY_CAM_CHECK(readSetupMembers(*args, e.cam->setups[*index], false, *e.cam));
    RAY_CAM_CHECK(checkSetup(*args, *e.doc, *e.cam, *index));
    RAY_CAM_CHECK(checkStockChain(*args, *e.cam));
    return e.finish();
}

Result<DocStatePtr> setupRemove(const DocState& before, std::string_view argsJson)
{
    RAY_CAM_TRY(args, Args::parse(kSetupRemove, argsJson, {"id"}));
    Edit e = edit(before);
    RAY_CAM_TRY(index, existingSetup(*args, *e.cam));
    const cam::Setup& s = e.cam->setups[*index];
    for (const cam::Setup& other : e.cam->setups) {
        if (const auto* from = std::get_if<cam::FromSetupStock>(&other.stock); from && from->previous == s.id) {
            return args->error("id", "names setup '" + s.name + "', whose stock setup '" + other.name +
                                         "' starts from; change that setup's stock first");
        }
    }
    for (const OperationId& op : s.operations) {
        e.cam->operations.erase(op);
    }
    e.cam->setups.erase(e.cam->setups.begin() + static_cast<std::ptrdiff_t>(*index));
    return e.finish();
}

Result<DocStatePtr> setupReorder(const DocState& before, std::string_view argsJson)
{
    RAY_CAM_TRY(args, Args::parse(kSetupReorder, argsJson, {"id", "index"}));
    Edit e = edit(before);
    RAY_CAM_TRY(from, existingSetup(*args, *e.cam));
    RAY_CAM_TRY(to, args->optIndex("index", e.cam->setups.size() - 1));
    if (!*to) {
        return args->error("index", "is required");
    }
    auto& setups = e.cam->setups;
    cam::Setup moved = std::move(setups[*from]);
    setups.erase(setups.begin() + static_cast<std::ptrdiff_t>(*from));
    setups.insert(setups.begin() + static_cast<std::ptrdiff_t>(**to), std::move(moved));
    RAY_CAM_CHECK(checkStockChain(*args, *e.cam));
    return e.finish();
}

// ---------- cam.operation.* ----------

// The operation members of add and edit, read onto `op` (`base` for edits).
Status readOperationMembers(const Args& args, cam::Operation& op, bool adding, const cam::CamState& cam)
{
    const Source source = argsSource(args);
    RAY_CAM_TRY(type, adding ? args.text("type").map([](std::string v) { return std::optional<std::string>(v); })
                             : args.optText("type"));
    if (*type) {
        if (!validTypeName(**type)) {
            return args.error("type", "must be 1 to 64 of the characters A-Z a-z 0-9 _ . -");
        }
        op.type = std::move(**type);
    }
    RAY_CAM_TRY(name, adding ? args.text("name").map([](std::string v) { return std::optional<std::string>(v); })
                             : args.optText("name"));
    if (*name) {
        op.name = std::move(**name);
    }
    RAY_CAM_TRY(tool, args.optValue("tool"));
    if (*tool != nullptr) {
        RAY_CAM_TRY(read, camjson::readToolRef(**tool, source, "tool", adding ? nullptr : &op.tool, &cam));
        op.tool = *read;
    } else if (adding) {
        return args.error("tool", "is required");
    }
    RAY_CAM_TRY(cutting, args.optValue("cutting"));
    if (*cutting != nullptr) {
        RAY_CAM_TRY(read, camjson::readCutting(**cutting, source, "cutting", adding ? nullptr : &op.cutting));
        op.cutting = *read;
    } else if (adding) {
        return args.error("cutting", "is required");
    }
    RAY_CAM_TRY(heights, args.optValue("heights"));
    if (*heights != nullptr) {
        RAY_CAM_TRY(read, camjson::readHeights(**heights, source, "heights", adding ? nullptr : &op.heights));
        op.heights = *read;
    } else if (adding) {
        return args.error("heights", "is required");
    }
    RAY_CAM_TRY(geometry, args.optValue("geometry"));
    if (*geometry != nullptr) {
        RAY_CAM_TRY(refs, camjson::readGeometry(**geometry, source, "geometry"));
        op.geometry = std::move(*refs);
    }
    RAY_CAM_TRY(params, args.optObject("params"));
    if (*params) {
        op.paramsJson = std::move(**params);
    } else if (adding) {
        op.paramsJson = "{}";
    }
    RAY_CAM_TRY(templ, args.optString("template"));
    if (*templ) {
        op.templateId = std::move(**templ);
    }
    return {};
}

Result<DocStatePtr> operationAdd(const DocState& before, std::string_view argsJson)
{
    RAY_CAM_TRY(args, Args::parse(kOpAdd, argsJson,
                                  {"cutting", "geometry", "heights", "id", "index", "library", "name", "params",
                                   "setup", "suppressed", "template", "tool", "type"}));
    RAY_CAM_TRY(id, args->id("id"));
    Edit e = edit(before);
    if (e.cam->operations.count(OperationId{*id}) != 0) {
        return args->error("id", "is already used by an operation (" + id->toString() + ")");
    }
    RAY_CAM_TRY(setup, existingSetup(*args, *e.cam, "setup"));
    RAY_CAM_TRY(index, args->optIndex("index", e.cam->setups[*setup].operations.size()));
    RAY_CAM_TRY(suppressed, args->optBool("suppressed"));
    RAY_CAM_CHECK(embedFromArgs(*args, *e.cam));
    cam::Operation op;
    op.id = OperationId{*id};
    op.suppressed = suppressed->value_or(false);
    RAY_CAM_CHECK(readOperationMembers(*args, op, true, *e.cam));
    cam::Setup& s = e.cam->setups[*setup];
    RAY_CAM_CHECK(checkOperation(*args, *e.cam, op, s));
    const std::size_t at = index->value_or(s.operations.size());
    s.operations.insert(s.operations.begin() + static_cast<std::ptrdiff_t>(at), op.id);
    e.cam->operations.emplace(op.id, std::move(op));
    return e.finish();
}

Result<DocStatePtr> operationEdit(const DocState& before, std::string_view argsJson)
{
    RAY_CAM_TRY(args, Args::parse(kOpEdit, argsJson,
                                  {"cutting", "geometry", "heights", "id", "library", "name", "params", "template",
                                   "tool", "type"}));
    Edit e = edit(before);
    RAY_CAM_TRY(id, existingOperation(*args, *e.cam));
    if (!args->has("cutting") && !args->has("geometry") && !args->has("heights") && !args->has("library") &&
        !args->has("name") && !args->has("params") && !args->has("template") && !args->has("tool") &&
        !args->has("type")) {
        return args->error("nothing to change: give at least one member of the operation");
    }
    RAY_CAM_CHECK(embedFromArgs(*args, *e.cam));
    cam::Operation& op = e.cam->operations.at(*id);
    RAY_CAM_CHECK(readOperationMembers(*args, op, false, *e.cam));
    const std::ptrdiff_t setup = setupOfOp(*e.cam, *id);
    if (setup < 0) {
        return args->error("id", "names an operation that no setup lists");
    }
    RAY_CAM_CHECK(checkOperation(*args, *e.cam, op, e.cam->setups[static_cast<std::size_t>(setup)]));
    return e.finish();
}

Result<DocStatePtr> operationRemove(const DocState& before, std::string_view argsJson)
{
    RAY_CAM_TRY(args, Args::parse(kOpRemove, argsJson, {"id"}));
    Edit e = edit(before);
    RAY_CAM_TRY(id, existingOperation(*args, *e.cam));
    for (cam::Setup& s : e.cam->setups) {
        s.operations.erase(std::remove(s.operations.begin(), s.operations.end(), *id), s.operations.end());
    }
    e.cam->operations.erase(*id);
    return e.finish();
}

Result<DocStatePtr> operationReorder(const DocState& before, std::string_view argsJson)
{
    RAY_CAM_TRY(args, Args::parse(kOpReorder, argsJson, {"id", "index", "setup"}));
    Edit e = edit(before);
    RAY_CAM_TRY(id, existingOperation(*args, *e.cam));
    std::size_t at = 0;
    const std::ptrdiff_t from = setupOfOp(*e.cam, *id, &at);
    if (from < 0) {
        return args->error("id", "names an operation that no setup lists");
    }
    std::size_t target = static_cast<std::size_t>(from);
    if (args->has("setup")) {
        RAY_CAM_TRY(to, existingSetup(*args, *e.cam, "setup"));
        target = *to;
    }
    auto& source = e.cam->setups[static_cast<std::size_t>(from)].operations;
    source.erase(source.begin() + static_cast<std::ptrdiff_t>(at));
    auto& list = e.cam->setups[target].operations;
    RAY_CAM_TRY(index, args->optIndex("index", list.size()));
    if (!*index) {
        return args->error("index", "is required");
    }
    list.insert(list.begin() + static_cast<std::ptrdiff_t>(**index), *id);
    if (target != static_cast<std::size_t>(from)) {
        RAY_CAM_CHECK(checkOperation(*args, *e.cam, e.cam->operations.at(*id), e.cam->setups[target]));
    }
    return e.finish();
}

Result<DocStatePtr> operationSuppress(const DocState& before, std::string_view argsJson)
{
    RAY_CAM_TRY(args, Args::parse(kOpSuppress, argsJson, {"id", "suppressed"}));
    Edit e = edit(before);
    RAY_CAM_TRY(id, existingOperation(*args, *e.cam));
    RAY_CAM_TRY(suppressed, args->boolean("suppressed"));
    e.cam->operations.at(*id).suppressed = *suppressed;
    return e.finish();
}

// ---------- cam.library.embed ----------

Result<DocStatePtr> libraryEmbed(const DocState& before, std::string_view argsJson)
{
    RAY_CAM_TRY(args, Args::parse(kLibraryEmbed, argsJson, {"holders", "machines", "materials", "tools"}));
    Edit e = edit(before);
    auto parsed = json::parse(argsJson);
    if (!parsed || parsed->asObject().empty()) {
        return args->error("nothing to embed: give tools, holders, machines or materials");
    }
    RAY_CAM_CHECK(embedLibrary(*args, *parsed, *e.cam, ""));
    return e.finish();
}

#undef RAY_CAM_TRY
#undef RAY_CAM_CHECK

// ---------- Argument schemas (JSON Schema 2020-12, the subset src/api/schema.h validates) ----------

std::string uuid()
{
    return features::uuidSchema();
}

std::string nullableUuid()
{
    return R"({"anyOf":[)" + uuid() + R"(,{"type":"null"}]})";
}

std::string number(std::string_view description, std::string_view unit = "length")
{
    return R"({"type":"number","description":")" + std::string(description) + R"(","x-unit":")" + std::string(unit) +
           R"("})";
}

std::string bodies(std::string_view description)
{
    return R"({"type":"array","items":)" + uuid() + R"(,"description":")" + std::string(description) + R"("})";
}

std::string refs()
{
    return R"({"type":"array","items":)" + features::topoRefSchema() +
           R"(,"description":"Faces, edges or hole cylinders the operation machines; model.reference makes them"})";
}

std::string stockSchema()
{
    return R"({"type":"object","required":["type"],"description":"Stock: a box grown from the part's bounding box )"
           R"(by offsets, a cylinder, a solid body, or the stock an earlier setup leaves","properties":{"type":)"
           R"({"enum":["box","cylinder","solid","fromSetup"]},"xNeg":)" +
           number("mm") + R"(,"xPos":)" + number("mm") + R"(,"yNeg":)" + number("mm") + R"(,"yPos":)" + number("mm") +
           R"(,"zNeg":)" + number("mm") + R"(,"zPos":)" + number("mm") + R"(,"diameter":)" + number("mm") +
           R"(,"length":)" + number("mm") + R"(,"endOffset":)" + number("mm") +
           R"(,"axis":{"enum":["x","y","z"]},"body":)" + uuid() + R"(,"setup":)" + uuid() + "}}";
}

std::string wcsSchema()
{
    return R"({"type":"object","additionalProperties":false,"description":"Work coordinate system","properties":{)"
           R"("origin":{"type":"object","required":["mode"],"properties":{"mode":{"enum":["stockCorner",)"
           R"("stockTopCenter","modelPoint"]},"corner":{"type":"integer","minimum":0,"maximum":7}}},)"
           R"("workOffset":{"type":"integer","minimum":1,"maximum":99,"description":"1 is G54"},)"
           R"("flipX":{"type":"boolean","description":"Reverses X (and Y): X follows the X edge's curve direction"},)"
           R"("flipZ":{"type":"boolean","description":"Reverses Z (into the material); X follows its rule for it"},)"
           R"("zFace":)" +
           features::topoRefSchema() + R"(,"xEdge":)" + features::topoRefSchema() + R"(,"originVertex":)" +
           features::topoRefSchema() + "}}";
}

std::string toolRefSchema()
{
    return R"({"type":"object","additionalProperties":false,"properties":{"tool":)" + uuid() + R"(,"holder":)" +
           nullableUuid() +
           R"(,"number":{"type":"integer","minimum":0},"lengthOffset":{"type":"integer","minimum":0},)"
           R"("diameterOffset":{"type":"integer","minimum":0},"stickOut":)" +
           number("Tool stick-out from the holder, mm") + "}}";
}

std::string cuttingSchema()
{
    return R"({"type":"object","additionalProperties":false,"properties":{"rpm":{"type":"number","exclusiveMinimum":0},)"
           R"("clockwise":{"type":"boolean"},"feedCut":{"type":"number","exclusiveMinimum":0,"description":"mm/min"},)"
           R"("feedPlunge":{"type":"number","exclusiveMinimum":0},"feedRamp":{"type":"number","exclusiveMinimum":0},)"
           R"("feedLeadIn":{"type":"number","exclusiveMinimum":0},"feedLeadOut":{"type":"number","exclusiveMinimum":0},)"
           R"("coolant":{"enum":["off","flood","mist","throughTool","air"]}}})";
}

std::string heightsSchema()
{
    return R"({"type":"object","additionalProperties":false,"properties":{"clearance":)" + number("mm") +
           R"(,"retract":)" + number("mm") + R"(,"feedPlane":)" + number("mm") + R"(,"top":)" + number("mm") +
           R"(,"bottom":)" + number("mm") + "}}";
}

std::string librarySchema()
{
    const std::string item = R"({"type":"array","items":{"type":"object","required":["id","name"]}})";
    return R"({"type":"object","additionalProperties":false,"description":"Library items to embed in the )"
           R"(document: a copy of every tool, holder, machine and material it uses","properties":{"tools":)" +
           item + R"(,"holders":)" + item + R"(,"machines":)" + item + R"(,"materials":)" + item + "}}";
}

std::string setupProperties(bool withIndex)
{
    return R"("id":)" + uuid() + R"(,"name":{"type":"string","minLength":1},"partBodies":)" +
           bodies("The bodies this setup machines") + R"(,"fixtureBodies":)" +
           bodies("Vises, clamps and plates: collision bodies, never cut") + R"(,"stock":)" + stockSchema() +
           R"(,"wcs":)" + wcsSchema() + R"(,"machine":)" + nullableUuid() + R"(,"post":{"type":"string"},)" +
           R"("programNumber":{"type":"integer","minimum":0,"maximum":99999999},"programComment":{"type":"string"},)" +
           R"("library":)" + librarySchema() + (withIndex ? R"(,"index":{"type":"integer","minimum":0})" : "");
}

std::string operationProperties()
{
    return R"("id":)" + uuid() + R"(,"type":{"type":"string","pattern":"^[A-Za-z0-9_.-]{1,64}$"},)" +
           R"("name":{"type":"string","minLength":1},"tool":)" + toolRefSchema() + R"(,"cutting":)" + cuttingSchema() +
           R"(,"heights":)" + heightsSchema() + R"(,"geometry":)" + refs() +
           R"(,"params":{"type":"object","description":"Type-specific; the operation type's schema"},)" +
           R"("template":{"type":"string"},"library":)" + librarySchema();
}

std::string head(std::string_view required, std::string_view properties, bool minTwo = false)
{
    return R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object","additionalProperties":false,)" +
           std::string(R"("required":[)") + std::string(required) + "]," + (minTwo ? R"("minProperties":2,)" : "") +
           R"("properties":{)" + std::string(properties) + "}}";
}

} // namespace

std::vector<std::unique_ptr<ICommandHandler>> makeCamCommandHandlers()
{
    std::vector<std::unique_ptr<ICommandHandler>> out;
    const std::string id = R"("id":)" + uuid();
    const std::string index = R"("index":{"type":"integer","minimum":0})";
    out.push_back(
        std::make_unique<CamCommand>(kSetupAdd, head(R"("id","name","partBodies")", setupProperties(true)), &setupAdd));
    out.push_back(std::make_unique<CamCommand>(kSetupEdit, head(R"("id")", setupProperties(false), true), &setupEdit));
    out.push_back(std::make_unique<CamCommand>(kSetupRemove, head(R"("id")", id), &setupRemove));
    out.push_back(
        std::make_unique<CamCommand>(kSetupReorder, head(R"("id","index")", id + "," + index), &setupReorder));
    out.push_back(std::make_unique<CamCommand>(
        kOpAdd,
        head(R"("id","setup","type","name","tool","cutting","heights")",
             operationProperties() + R"(,"setup":)" + uuid() + R"(,"suppressed":{"type":"boolean"},)" + index),
        &operationAdd));
    out.push_back(std::make_unique<CamCommand>(kOpEdit, head(R"("id")", operationProperties(), true), &operationEdit));
    out.push_back(std::make_unique<CamCommand>(kOpRemove, head(R"("id")", id), &operationRemove));
    out.push_back(std::make_unique<CamCommand>(
        kOpReorder, head(R"("id","index")", id + "," + index + R"(,"setup":)" + uuid()), &operationReorder));
    out.push_back(std::make_unique<CamCommand>(
        kOpSuppress, head(R"("id","suppressed")", id + R"(,"suppressed":{"type":"boolean"})"), &operationSuppress));
    const std::string item = R"({"type":"array","items":{"type":"object","required":["id","name"]}})";
    out.push_back(std::make_unique<CamCommand>(
        kLibraryEmbed,
        R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object","additionalProperties":false,)"
        R"("minProperties":1,"properties":{"tools":)" +
            item + R"(,"holders":)" + item + R"(,"machines":)" + item + R"(,"materials":)" + item + "}}",
        &libraryEmbed));
    return out;
}

} // namespace commands
} // namespace ray::doc
