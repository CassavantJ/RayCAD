// Posting a setup (ray_api/cam_post.h, task T-047): the gate, the post run, the re-check before the file is put in
// place, the file output (src/post/output.h) and the settings remembered per setup.
#include "../core/from_chars_double.h"
#include "../post/output.h"
#include "files.h"
#include "json.h"

#include <ray_api/cam_post.h>
#include <ray_api/cam_setup.h>
#include <ray_api/settings.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <system_error>

namespace ray::api::cam_post {

namespace {

using json::Value;

Error failure(ErrorCode code, std::string message)
{
    return Error{code, std::move(message), {}};
}

const cam::Setup* findSetup(const doc::DocState& state, const SetupId& id)
{
    if (!state.cam)
        return nullptr;
    const auto it = std::find_if(state.cam->setups.begin(), state.cam->setups.end(),
                                 [&](const cam::Setup& s) { return s.id == id; });
    return it == state.cam->setups.end() ? nullptr : &*it;
}

const lib::Tool* toolOf(const cam::CamState& cam, const cam::Operation& op)
{
    const auto it = cam.tools.find(op.tool.tool);
    return it == cam.tools.end() ? nullptr : &it->second.item;
}

const lib::Holder* holderOf(const cam::CamState& cam, const cam::Operation& op)
{
    const auto it = cam.holders.find(op.tool.holder);
    return it == cam.holders.end() ? nullptr : &it->second.item;
}

// "<what>: <why>. <fix>", without doubled punctuation when <why> ends a sentence itself.
std::string sentences(const std::string& what, const std::string& why, const std::string& fix)
{
    std::string out = what;
    if (!why.empty()) {
        out += ": " + why;
        const char last = why.back();
        out += last == '.' || last == ')' || last == '!' || last == '?' ? " " : ". ";
    } else {
        out += ". ";
    }
    return out + fix;
}

std::string percent(double fraction)
{
    if (!(fraction >= 0))
        return {};
    return " (" + std::to_string(static_cast<int>(std::lround(std::min(1.0, fraction) * 100))) + "%)";
}

// A number as the post host reads it: the shortest text that reads back as the same double.
std::string numberText(double d)
{
    if (std::floor(d) == d && std::fabs(d) < 9.0e15)
        return std::to_string(static_cast<long long>(d));
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, d);
    return std::string(buf, r.ptr);
}

std::optional<double> parseDouble(std::string_view text)
{
    double d = 0;
    const auto r = compat::fromChars(text.data(), text.data() + text.size(), d);
    if (r.ec != std::errc() || r.ptr != text.data() + text.size())
        return std::nullopt;
    return d;
}

// A property's value as JSON of its type, from the text the host reads; null when it does not read.
Value typedValue(const post::PostProperty& p, std::string_view text)
{
    switch (p.type) {
    case post::PostProperty::Type::Bool:
        if (text == "true" || text == "1")
            return Value::boolean(true);
        if (text == "false" || text == "0")
            return Value::boolean(false);
        return Value();
    case post::PostProperty::Type::Int:
    case post::PostProperty::Type::Number:
        if (const auto d = parseDouble(text))
            return p.type == post::PostProperty::Type::Int ? Value::integer(static_cast<std::int64_t>(*d))
                                                           : Value::number(*d);
        return Value();
    case post::PostProperty::Type::Enum:
    case post::PostProperty::Type::String:
        return Value::string(std::string(text));
    }
    return Value();
}

// The text the host reads, from a JSON value of the property's type; nullopt when it is not of that type.
std::optional<std::string> textOf(const post::PostProperty& p, const Value& v)
{
    switch (p.type) {
    case post::PostProperty::Type::Bool:
        if (v.isBool())
            return v.asBool() ? std::string("true") : std::string("false");
        if (v.isString() && (v.asString() == "true" || v.asString() == "false"))
            return v.asString();
        return std::nullopt;
    case post::PostProperty::Type::Int:
    case post::PostProperty::Type::Number:
        if (v.isNumber())
            return numberText(v.asNumber());
        if (v.isString() && parseDouble(v.asString()))
            return numberText(*parseDouble(v.asString()));
        return std::nullopt;
    case post::PostProperty::Type::Enum:
    case post::PostProperty::Type::String:
        if (v.isString())
            return v.asString();
        if (v.isNumber())
            return numberText(v.asNumber());
        if (v.isBool())
            return v.asBool() ? std::string("true") : std::string("false");
        return std::nullopt;
    }
    return std::nullopt;
}

// The default in effect: the machine's override (its JSON text, libraries.h) where it has one, else the declared one.
Value defaultOf(const post::PostProperty& p, const lib::MachineDef* machine)
{
    if (machine != nullptr) {
        if (const auto it = machine->postPropertyOverrides.find(p.key); it != machine->postPropertyOverrides.end()) {
            if (auto parsed = json::parse(it->second)) {
                if (auto text = textOf(p, *parsed)) {
                    Value v = typedValue(p, *text);
                    if (!v.isNull())
                        return v;
                }
            }
        }
    }
    Value v = typedValue(p, p.defaultValue);
    return v.isNull() ? Value::string(p.defaultValue) : v;
}

bool sameValue(const Value& a, const Value& b)
{
    if (a.isNumber() && b.isNumber())
        return a.asNumber() == b.asNumber();
    return a == b;
}

std::string stripRaycad(std::string name)
{
    constexpr std::string_view kExt = ".raycad";
    if (name.size() > kExt.size() && name.compare(name.size() - kExt.size(), kExt.size(), kExt) == 0)
        name.resize(name.size() - kExt.size());
    return name;
}

std::string partNameOf(const doc::DocState& state, const cam::Setup& setup)
{
    for (const BodyId& id : setup.partBodies) {
        for (const doc::Body& b : state.bodies) {
            if (b.id == id)
                return b.name;
        }
    }
    return {};
}

post::OverwriteMode modeOf(Overwrite o)
{
    switch (o) {
    case Overwrite::Replace:
        return post::OverwriteMode::Replace;
    case Overwrite::NewName:
        return post::OverwriteMode::NewName;
    case Overwrite::Ask:
    case Overwrite::Refuse:
        return post::OverwriteMode::Refuse;
    }
    return post::OverwriteMode::Refuse;
}

FileName fromPost(const post::FileName& f)
{
    return FileName{f.name, f.stem, f.extension, f.notes};
}

int stemLimitOf(const lib::MachineDef* machine, const Settings& settings)
{
    return settings.limitNameLength && machine != nullptr && machine->maxProgramNameLength > 0
               ? machine->maxProgramNameLength
               : 0;
}

Result<FileName> nameOf(const doc::DocState& state, const cam::Setup& setup, const lib::MachineDef* machine,
                        const post::PostInfo& info, const Settings& settings, std::string_view documentName)
{
    post::NameFields f;
    f.programNumber = settings.programNumber.value_or(setup.programNumber);
    f.programComment = settings.programComment.value_or(setup.programComment);
    f.setupName = setup.name;
    f.documentName = std::string(documentName);
    f.partName = partNameOf(state, setup);
    if (f.partName.empty())
        f.partName = f.documentName;
    f.postId = info.id;
    f.machineName = machine != nullptr ? machine->name : std::string();
    f.extension = info.fileExtension;
    auto name = post::fileNameFor(settings.fileNamePattern.empty() ? std::string_view(kDefaultFileNamePattern)
                                                                   : std::string_view(settings.fileNamePattern),
                                  f, stemLimitOf(machine, settings));
    if (!name)
        return tl::unexpected(name.error());
    return fromPost(*name);
}

} // namespace

// ---------- The gate ----------
std::string_view reasonKey(BlockReason reason)
{
    switch (reason) {
    case BlockReason::NotBuilt:
        return "notBuilt";
    case BlockReason::NoSetup:
        return "noSetup";
    case BlockReason::SetupBroken:
        return "setupBroken";
    case BlockReason::NoMachine:
        return "noMachine";
    case BlockReason::NoOperations:
        return "noOperations";
    case BlockReason::Stale:
        return "stale";
    case BlockReason::Generating:
        return "generating";
    case BlockReason::Failed:
        return "failed";
    case BlockReason::ReferenceBroken:
        return "referenceBroken";
    case BlockReason::NoToolpath:
        return "noToolpath";
    case BlockReason::NoTool:
        return "noTool";
    }
    return "unknown";
}

std::vector<Blocker> blockers(const doc::DocState& state, bool built, const SetupId& setupId,
                              const std::map<OperationId, cam::OpState>& statuses, const Status& setupState)
{
    std::vector<Blocker> out;
    const cam::Setup* setup = findSetup(state, setupId);
    if (setup == nullptr) {
        out.push_back(
            Blocker{OperationId{}, std::string(), BlockReason::NoSetup, "The document has no such setup", false});
        return out;
    }
    if (!built) {
        out.push_back(Blocker{OperationId{}, setup->name, BlockReason::NotBuilt,
                              "The model is not built yet; wait until it is", false});
        return out;
    }
    if (!setupState) {
        out.push_back(Blocker{OperationId{}, setup->name, BlockReason::SetupBroken,
                              "The setup's work offset or stock does not resolve: " + setupState.error().message +
                                  ". Edit the setup and pick its references again",
                              false});
    }
    const cam::CamState& cam = *state.cam;
    if (cam.machines.find(setup->machine) == cam.machines.end()) {
        out.push_back(Blocker{OperationId{}, setup->name, BlockReason::NoMachine,
                              "The setup has no machine; choose one in the setup", false});
    }
    int posted = 0;
    for (const OperationId& id : setup->operations) {
        const auto op = cam.operations.find(id);
        if (op == cam.operations.end() || op->second.suppressed)
            continue;
        const std::string& name = op->second.name;
        const auto st = statuses.find(id);
        const cam::OpStatus status = st == statuses.end() ? cam::OpStatus::Stale : st->second.status;
        if (status == cam::OpStatus::Suppressed)
            continue;
        ++posted;
        const std::string engineMessage = st == statuses.end() ? std::string() : st->second.message;
        switch (status) {
        case cam::OpStatus::UpToDate:
            if (!st->second.toolpath)
                out.push_back(Blocker{id, name, BlockReason::NoToolpath, "Its toolpath has not been generated", true});
            break;
        case cam::OpStatus::Stale:
            if (st != statuses.end() && st->second.toolpath)
                out.push_back(Blocker{id, name, BlockReason::Stale,
                                      "Its toolpath is out of date: the operation, its tool or the model changed "
                                      "since it was generated",
                                      true});
            else
                out.push_back(Blocker{id, name, BlockReason::NoToolpath, "Its toolpath has not been generated", true});
            break;
        case cam::OpStatus::Generating:
            out.push_back(Blocker{id, name, BlockReason::Generating,
                                  "Its toolpath is still being generated" + percent(st->second.progress) +
                                      "; wait until it is done",
                                  false});
            break;
        case cam::OpStatus::Failed:
            out.push_back(Blocker{id, name, BlockReason::Failed,
                                  sentences("Generating its toolpath failed", engineMessage, "Edit the operation"),
                                  false});
            break;
        case cam::OpStatus::ReferenceBroken:
            out.push_back(Blocker{id, name, BlockReason::ReferenceBroken,
                                  sentences("A geometry reference does not resolve", engineMessage,
                                            "Edit the operation and pick its geometry again"),
                                  false});
            break;
        case cam::OpStatus::Suppressed:
            break;
        }
        if (toolOf(cam, op->second) == nullptr)
            out.push_back(
                Blocker{id, name, BlockReason::NoTool, "Its tool is not in the document; choose a tool", false});
    }
    if (posted == 0)
        out.push_back(Blocker{OperationId{}, setup->name, BlockReason::NoOperations,
                              setup->operations.empty() ? "The setup has no operations"
                                                        : "Every operation of the setup is suppressed",
                              false});
    return out;
}

std::string blockersText(const std::vector<Blocker>& list)
{
    std::string text = "Posting is blocked:";
    for (const Blocker& b : list)
        text += "\n" + (b.name.empty() ? std::string("Setup") : b.name) + ": " + b.message;
    return text;
}

std::string problemKey(const Error& error)
{
    if (error.context.empty() || error.context.front().rfind("output.", 0) != 0)
        return error.code == ErrorCode::Cancelled ? std::string("cancelled") : std::string();
    return error.context.front().substr(7);
}

// ---------- Posts and their properties ----------
std::string findPost(const std::vector<std::string>& roots, std::string_view id)
{
    if (id.empty() || id.find_first_of("/\\") != std::string_view::npos || id.find("..") != std::string_view::npos)
        return {};
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
        std::error_code ec;
        const std::filesystem::path p =
            detail::toFsPath(*it) / "posts" / detail::toFsPath(std::string(id) + ".post.js");
        if (std::filesystem::is_regular_file(p, ec))
            return detail::fromFsPath(p);
    }
    return {};
}

Result<post::PostInfo> inspectPost(const std::string& scriptPath)
{
    auto engine = post::makeQuickJsPostEngine();
    if (!engine)
        return tl::unexpected(failure(ErrorCode::Internal, "The post engine could not start"));
    return engine->inspect(scriptPath);
}

std::string propertiesSchemaJson(const post::PostInfo& info, const lib::MachineDef* machine)
{
    Value order = Value::array();
    Value props = Value::object();
    for (const post::PostProperty& p : info.properties) {
        Value prop = Value::object();
        prop.set("title", Value::string(p.title.empty() ? p.key : p.title));
        std::string description = p.description;
        if (!p.group.empty())
            description = description.empty() ? p.group : p.group + ": " + description;
        if (!description.empty())
            prop.set("description", Value::string(description));
        switch (p.type) {
        case post::PostProperty::Type::Bool:
            prop.set("type", Value::string("boolean"));
            break;
        case post::PostProperty::Type::Int:
            prop.set("type", Value::string("integer"));
            break;
        case post::PostProperty::Type::Number:
            prop.set("type", Value::string("number"));
            break;
        case post::PostProperty::Type::Enum: {
            prop.set("type", Value::string("string"));
            Value values = Value::array();
            for (const std::string& v : p.enumValues)
                values.push(Value::string(v));
            prop.set("enum", std::move(values));
            break;
        }
        case post::PostProperty::Type::String:
            prop.set("type", Value::string("string"));
            break;
        }
        prop.set("default", defaultOf(p, machine));
        props.set(p.key, std::move(prop));
        order.push(Value::string(p.key));
    }
    Value schema = Value::object({{"type", Value::string("object")},
                                  {"title", Value::string(info.title)},
                                  {"x-propertyOrder", std::move(order)},
                                  {"properties", std::move(props)}});
    return json::write(schema);
}

std::map<std::string, std::string> propertyOverrides(const post::PostInfo& info, const lib::MachineDef* machine,
                                                     std::string_view valuesJson)
{
    std::map<std::string, std::string> out;
    auto values = json::parse(valuesJson);
    if (!values || !values->isObject())
        return out;
    for (const post::PostProperty& p : info.properties) {
        const Value* v = values->find(p.key);
        if (v == nullptr || v->isNull())
            continue;
        const auto text = textOf(p, *v);
        if (!text)
            continue;
        const Value typed = typedValue(p, *text);
        if (!typed.isNull() && sameValue(typed, defaultOf(p, machine)))
            continue;
        out[p.key] = *text;
    }
    return out;
}

std::string propertyValuesJson(const post::PostInfo& info, const std::map<std::string, std::string>& overrides)
{
    Value out = Value::object();
    for (const post::PostProperty& p : info.properties) {
        const auto it = overrides.find(p.key);
        if (it == overrides.end())
            continue;
        Value v = typedValue(p, it->second);
        if (!v.isNull())
            out.set(p.key, std::move(v));
    }
    return json::write(out);
}

// ---------- Settings ----------
std::string_view overwriteKey(Overwrite o)
{
    switch (o) {
    case Overwrite::Ask:
        return "ask";
    case Overwrite::Replace:
        return "replace";
    case Overwrite::NewName:
        return "newName";
    case Overwrite::Refuse:
        return "refuse";
    }
    return "ask";
}

std::optional<Overwrite> overwriteFromKey(std::string_view key)
{
    for (Overwrite o : {Overwrite::Ask, Overwrite::Replace, Overwrite::NewName, Overwrite::Refuse}) {
        if (overwriteKey(o) == key)
            return o;
    }
    return std::nullopt;
}

const lib::MachineDef* machineOf(const doc::DocState& state, const SetupId& setupId)
{
    const cam::Setup* setup = findSetup(state, setupId);
    if (setup == nullptr)
        return nullptr;
    const auto it = state.cam->machines.find(setup->machine);
    return it == state.cam->machines.end() ? nullptr : &it->second.item;
}

Result<FileName> fileNameFor(const doc::DocState& state, const SetupId& setupId, const post::PostInfo& info,
                             const Settings& settings, std::string_view documentName)
{
    const cam::Setup* setup = findSetup(state, setupId);
    if (setup == nullptr)
        return tl::unexpected(failure(ErrorCode::NotFound, "The document has no such setup"));
    return nameOf(state, *setup, machineOf(state, setupId), info, settings, documentName);
}

// ---------- Posting ----------
Result<Posted> run(const Inputs& in, const Settings& settings, JobContext& ctx)
{
    if (!in.state || !in.state->cam)
        return tl::unexpected(failure(ErrorCode::NotFound, "The document has no CAM setups"));
    const doc::DocState& state = *in.state;
    const cam::Setup* setup = findSetup(state, in.setup);
    if (setup == nullptr)
        return tl::unexpected(failure(ErrorCode::NotFound, "The document has no such setup"));
    ctx.progress.set(0.02, "Checking the setup");
    // The setup's own resolution (its WCS and stock references), as the CAM tree shows it.
    Status setupState = std::monostate{};
    if (in.build) {
        const std::map<SetupId, Status> states = cam_setup::setupStates(in.state, in.build, ctx);
        if (const auto it = states.find(in.setup); it != states.end())
            setupState = it->second;
    }
    if (auto ok = ctx.cancel.check(); !ok)
        return tl::unexpected(ok.error());
    // THE GATE: nothing reaches the post engine while anything blocks.
    const std::vector<Blocker> blocked = blockers(state, in.build != nullptr, in.setup, in.statuses, setupState);
    if (!blocked.empty())
        return tl::unexpected(failure(ErrorCode::PostFailed, blockersText(blocked)));

    const cam::CamState& cam = *state.cam;
    const lib::MachineDef* machine = machineOf(state, in.setup);
    std::string postId = settings.postId;
    if (postId.empty())
        postId = setup->postId;
    if (postId.empty() && machine != nullptr)
        postId = machine->defaultPost;
    if (postId.empty())
        return tl::unexpected(failure(ErrorCode::InvalidArgument, "Choose a post: neither the setup nor its machine "
                                                                  "names one"));
    const std::string script = findPost(in.libraryRoots, postId);
    if (script.empty())
        return tl::unexpected(failure(ErrorCode::NotFound, "The post '" + postId + "' is not in the library (posts/" +
                                                               postId +
                                                               ".post.js in the built-in, shared or user library)"));
    auto engine = post::makeQuickJsPostEngine();
    if (!engine)
        return tl::unexpected(failure(ErrorCode::Internal, "The post engine could not start"));
    auto info = engine->inspect(script);
    if (!info)
        return tl::unexpected(info.error());

    Posted posted;
    posted.basis = in.state;
    posted.info = *info;
    posted.scriptPath = script;
    posted.setup = in.setup;
    posted.setupName = setup->name;
    posted.machineName = machine != nullptr ? machine->name : std::string();
    posted.machineVerified = machine != nullptr && machine->verified;

    // The job: the setup as it is, with the program number and comment of this run.
    cam::Setup jobSetup = *setup;
    jobSetup.programNumber = settings.programNumber.value_or(setup->programNumber);
    jobSetup.programComment = settings.programComment.value_or(setup->programComment);
    post::PostJob job;
    job.setup = &jobSetup;
    job.machine = machine;
    job.outputUnits = settings.units.value_or(state.settings.units);
    job.properties = settings.properties;
    job.documentName = in.documentName;
    job.fixedDate = settings.date;
    for (const OperationId& id : setup->operations) {
        const auto op = cam.operations.find(id);
        if (op == cam.operations.end())
            continue;
        if (op->second.suppressed) {
            posted.notes.push_back(op->second.name + " is suppressed and was left out");
            continue;
        }
        const auto st = in.statuses.find(id);
        if (st == in.statuses.end() || st->second.status == cam::OpStatus::Suppressed) {
            if (st != in.statuses.end())
                posted.notes.push_back(op->second.name + " is suppressed and was left out");
            continue;
        }
        post::PostSection section{&op->second, st->second.toolpath.get(), toolOf(cam, op->second),
                                  holderOf(cam, op->second)};
        job.sections.push_back(section);
        posted.sectionOps.push_back(id);
        posted.sectionNames.push_back(op->second.name);
        posted.sectionInputs.push_back(st->second.toolpath->inputHash);
    }
    ctx.progress.set(0.1, "Posting");
    auto output = engine->run(script, job, ctx);
    if (!output)
        return tl::unexpected(output.error());
    posted.output = std::move(*output);
    ctx.progress.set(0.95, "Posted");

    Settings named = settings;
    named.programNumber = jobSetup.programNumber;
    named.programComment = jobSetup.programComment;
    auto name = nameOf(state, *setup, machine, *info, named, in.documentName);
    if (!name)
        return tl::unexpected(name.error());
    posted.fileName = std::move(*name);
    posted.maxStemLength = stemLimitOf(machine, settings);
    for (const std::string& note : posted.fileName.notes)
        posted.notes.push_back(note);
    return posted;
}

Status stillCurrent(const Posted& posted, const doc::DocState& now, bool built,
                    const std::map<OperationId, cam::OpState>& nowStatuses, const Status& setupState)
{
    const std::vector<Blocker> blocked = blockers(now, built, posted.setup, nowStatuses, setupState);
    if (!blocked.empty())
        return tl::unexpected(
            failure(ErrorCode::PostFailed,
                    "The setup changed while it was posted; nothing was written. " + blockersText(blocked)));
    const auto changed = [](const std::string& what) -> Status {
        return tl::unexpected(failure(ErrorCode::PostFailed, "The setup changed while it was posted (" + what +
                                                                 "); nothing was written. Post it again"));
    };
    const cam::Setup* setup = findSetup(now, posted.setup);
    const cam::Setup* before = posted.basis ? findSetup(*posted.basis, posted.setup) : nullptr;
    if (setup == nullptr || before == nullptr)
        return changed("the setup was removed");
    // What the post reads that the toolpaths' input hashes do not cover (cam.h operationInputHash "Not in it").
    if (setup->wcs.workOffset != before->wcs.workOffset || setup->machine != before->machine ||
        setup->name != before->name)
        return changed("its work offset, machine or name");
    const lib::MachineDef* m0 = machineOf(*posted.basis, posted.setup);
    const lib::MachineDef* m1 = machineOf(now, posted.setup);
    if ((m0 == nullptr) != (m1 == nullptr) || (m0 != nullptr && lib::toJson(*m0) != lib::toJson(*m1)))
        return changed("its machine definition");
    std::vector<OperationId> ops;
    for (const OperationId& id : setup->operations) {
        const auto op = now.cam->operations.find(id);
        const auto st = nowStatuses.find(id);
        if (op == now.cam->operations.end() || op->second.suppressed ||
            (st != nowStatuses.end() && st->second.status == cam::OpStatus::Suppressed))
            continue;
        ops.push_back(id);
    }
    if (ops != posted.sectionOps)
        return changed("its operations or their order");
    for (std::size_t i = 0; i < ops.size(); ++i) {
        const auto st = nowStatuses.find(ops[i]);
        if (st == nowStatuses.end() || st->second.status != cam::OpStatus::UpToDate || !st->second.toolpath ||
            st->second.toolpath->inputHash != posted.sectionInputs[i])
            return changed(posted.sectionNames[i] + " has another toolpath");
        const cam::Operation& a = posted.basis->cam->operations.at(ops[i]);
        const cam::Operation& b = now.cam->operations.at(ops[i]);
        if (a.name != b.name || a.tool.toolNumber != b.tool.toolNumber || a.tool.lengthOffset != b.tool.lengthOffset ||
            a.tool.diameterOffset != b.tool.diameterOffset || a.tool.tool != b.tool.tool ||
            a.tool.holder != b.tool.holder)
            return changed(posted.sectionNames[i] + ": its name, tool or offsets");
        const lib::Tool* t0 = toolOf(*posted.basis->cam, a);
        const lib::Tool* t1 = toolOf(*now.cam, b);
        if (t0 == nullptr || t1 == nullptr || lib::toJson(*t0) != lib::toJson(*t1))
            return changed(posted.sectionNames[i] + ": its tool");
    }
    return std::monostate{};
}

namespace {
post::OutputTarget targetOf(const Posted& posted, const Settings& settings)
{
    post::OutputTarget t;
    t.directory = settings.directory;
    t.fileName = posted.fileName.name;
    t.mode = modeOf(settings.overwrite);
    t.maxStemLength = posted.maxStemLength;
    return t;
}
} // namespace

Result<Written> write(const Posted& posted, const Settings& settings, post::IOutputFs* fs, CancelToken* cancel)
{
    if (settings.directory.empty())
        return tl::unexpected(post::outputError(ErrorCode::InvalidArgument, post::OutputProblem::FolderMissing,
                                                "Choose the folder to write the program to"));
    auto written = post::writeProgramFile(posted.output.text, targetOf(posted, settings),
                                          fs != nullptr ? *fs : post::localOutputFs(), cancel);
    if (!written)
        return tl::unexpected(written.error());
    return Written{written->path, written->fileName, written->replaced};
}

Result<bool> targetExists(const Posted& posted, const Settings& settings, post::IOutputFs* fs)
{
    if (settings.directory.empty())
        return tl::unexpected(post::outputError(ErrorCode::InvalidArgument, post::OutputProblem::FolderMissing,
                                                "Choose the folder to write the program to"));
    return post::targetExists(targetOf(posted, settings), fs != nullptr ? *fs : post::localOutputFs());
}

// ---------- Remembered per setup ----------
namespace {

constexpr std::size_t kMaxRemembered = 500;
std::mutex gRememberMutex;

std::string settingsFile(const std::string& userDataDir)
{
    return post::joinPath(userDataDir, "post-settings.json");
}

Value readSettingsFile(const std::string& userDataDir)
{
    std::ifstream in(detail::toFsPath(settingsFile(userDataDir)), std::ios::binary);
    if (!in)
        return Value::object();
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto parsed = json::parse(text);
    if (!parsed || !parsed->isObject())
        return Value::object();
    return std::move(*parsed);
}

Value commonOf(const Settings& s)
{
    return Value::object({{"directory", Value::string(s.directory)},
                          {"pattern", Value::string(s.fileNamePattern)},
                          {"overwrite", Value::string(std::string(overwriteKey(s.overwrite)))},
                          {"limitNameLength", Value::boolean(s.limitNameLength)}});
}

void readCommon(const Value& v, Settings& s)
{
    s.directory = v.stringOr("directory", s.directory);
    s.fileNamePattern = v.stringOr("pattern", s.fileNamePattern);
    if (auto o = overwriteFromKey(v.stringOr("overwrite", "")))
        s.overwrite = *o;
    s.limitNameLength = v.boolOr("limitNameLength", s.limitNameLength);
}

} // namespace

std::optional<Settings> rememberedSettings(const std::string& userDataDir, const SetupId& setup)
{
    if (userDataDir.empty())
        return std::nullopt;
    std::lock_guard<std::mutex> lock(gRememberMutex);
    const Value file = readSettingsFile(userDataDir);
    const Value* setups = file.find("setups");
    const Value* entry = setups != nullptr ? setups->find(setup.value.toString()) : nullptr;
    const Value* last = file.find("last");
    if ((entry == nullptr || !entry->isObject()) && (last == nullptr || !last->isObject()))
        return std::nullopt;
    Settings s;
    if (last != nullptr && last->isObject())
        readCommon(*last, s);
    if (entry != nullptr && entry->isObject()) {
        readCommon(*entry, s);
        s.postId = entry->stringOr("post", "");
        if (const Value* units = entry->find("units"); units != nullptr && units->isString())
            s.units = api::unitSystemFromName(units->asString());
        if (const Value* props = entry->find("properties"); props != nullptr && props->isObject()) {
            for (const auto& [k, v] : props->asObject()) {
                if (v.isString())
                    s.properties[k] = v.asString();
            }
        }
    }
    return s;
}

Status remember(const std::string& userDataDir, const SetupId& setup, const Settings& settings)
{
    if (userDataDir.empty())
        return std::monostate{};
    std::lock_guard<std::mutex> lock(gRememberMutex);
    Value file = readSettingsFile(userDataDir);
    Value* setups = file.find("setups");
    if (setups == nullptr || !setups->isObject()) {
        file.set("setups", Value::object());
        setups = file.find("setups");
    }
    double seq = file.numberOr("seq", 0) + 1;
    Value entry = commonOf(settings);
    entry.set("post", Value::string(settings.postId));
    if (settings.units)
        entry.set("units", Value::string(std::string(api::unitSystemName(*settings.units))));
    Value props = Value::object();
    for (const auto& [k, v] : settings.properties)
        props.set(k, Value::string(v));
    entry.set("properties", std::move(props));
    entry.set("seq", Value::number(seq));
    setups->set(setup.value.toString(), std::move(entry));
    // Bounded: the setups used longest ago go first.
    while (setups->asObject().size() > kMaxRemembered) {
        auto& members = setups->asObject();
        auto oldest = std::min_element(members.begin(), members.end(), [](const auto& a, const auto& b) {
            return a.second.numberOr("seq", 0) < b.second.numberOr("seq", 0);
        });
        members.erase(oldest);
    }
    file.set("format", Value::string("raycad.postSettings"));
    file.set("version", Value::integer(1));
    file.set("seq", Value::number(seq));
    file.set("last", commonOf(settings));
    std::error_code ec;
    std::filesystem::create_directories(detail::toFsPath(userDataDir), ec);
    post::OutputTarget target;
    target.directory = userDataDir;
    target.fileName = "post-settings.json";
    target.mode = post::OverwriteMode::Replace;
    auto written = post::writeProgramFile(json::canonical(file), target, post::localOutputFs());
    if (!written)
        return tl::unexpected(written.error());
    return std::monostate{};
}

} // namespace ray::api::cam_post
