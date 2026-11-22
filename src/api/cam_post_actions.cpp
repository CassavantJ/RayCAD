// The cam.post action (task T-047): posts a setup and writes the program file, for the command palette, scripts and
// agents. It goes through ray_api/cam_post.h exactly as the desktop's post dialog does, so the same gate refuses a
// stale, failed, generating or broken operation, and the same re-check runs just before the file is put in place.
// It changes G-code, so a script or an agent needs the user's confirmation first (ActionInfo::changesToolpathsOrGcode).
//
// Arguments: {"setup": "<uuid>", "directory": "<folder>", "document"?, "post"? (the setup's), "properties"? ({key:
// value}), "programNumber"?, "programComment"?, "units"? ("inch" | "metric"; the document's), "fileNamePattern"?
// ("{doc}_{setup}"), "overwrite"? ("refuse" | "replace" | "newName"; "refuse"), "limitNameLength"? (true)}.
// Result: {"path", "fileName", "replaced", "lines", "bytes", "hash"} and the post's warnings.
#include "cam_post_actions.h"

#include "app_impl.h"

#include <ray_api/cam_post.h>
#include <ray_api/cam_setup.h>
#include <ray_api/settings.h>

#include <charconv>
#include <cmath>

namespace ray::api::detail {
namespace {

using json::Value;

Value prop(const char* type, const char* description)
{
    return Value::object({{"type", Value::string(type)}, {"description", Value::string(description)}});
}

std::string schema()
{
    Value overwrite = prop("string", "What to do when the file exists: refuse (the default), replace it, or write "
                                     "the next free numbered name");
    overwrite.set("enum", Value::array({Value::string("refuse"), Value::string("replace"), Value::string("newName")}));
    overwrite.set("default", Value::string("refuse"));
    Value units = prop("string", "Output units; the document's when omitted");
    units.set("enum", Value::array({Value::string("inch"), Value::string("metric")}));
    Value number = prop("integer", "Program number; the setup's when omitted");
    number.set("minimum", Value::integer(0));
    number.set("maximum", Value::integer(99999999));
    Value pattern = prop("string", "File name pattern: {program}, {program:N}, {setup}, {doc}, {part}, {comment}, "
                                   "{post}, {machine}, {ext}");
    pattern.set("default", Value::string(std::string(cam_post::kDefaultFileNamePattern)));
    Value limit = prop("boolean", "Cut the file name to the machine's longest program name");
    limit.set("default", Value::boolean(true));
    Value setup = prop("string", "The setup's id");
    setup.set("pattern", Value::string("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
    Value props = Value::object({{"type", Value::string("object")},
                                 {"description", Value::string("Post property values by key; the machine's and the "
                                                               "post's defaults for the others")}});
    Value schema = Value::object(
        {{"$schema", Value::string("https://json-schema.org/draft/2020-12/schema")},
         {"type", Value::string("object")},
         {"additionalProperties", Value::boolean(false)},
         {"required", Value::array({Value::string("setup"), Value::string("directory")})},
         {"properties",
          Value::object({{"document", prop("string", "The OpenDocument id; the active document when omitted")},
                         {"setup", std::move(setup)},
                         {"directory", prop("string", "The folder to write the program to (local or a network "
                                                      "share)")},
                         {"post", prop("string", "The post's id (file stem); the setup's when omitted")},
                         {"properties", std::move(props)},
                         {"programNumber", std::move(number)},
                         {"programComment", prop("string", "Program comment; the setup's when omitted")},
                         {"units", std::move(units)},
                         {"fileNamePattern", std::move(pattern)},
                         {"overwrite", std::move(overwrite)},
                         {"limitNameLength", std::move(limit)}})}});
    return json::canonical(schema);
}

std::string valueText(const Value& v)
{
    if (v.isBool())
        return v.asBool() ? "true" : "false";
    if (v.isString())
        return v.asString();
    if (v.isNumber()) {
        const double d = v.asNumber();
        if (std::floor(d) == d && std::fabs(d) < 9.0e15)
            return std::to_string(static_cast<long long>(d));
        char buf[64];
        const auto r = std::to_chars(buf, buf + sizeof buf, d);
        return std::string(buf, r.ptr);
    }
    return json::write(v);
}

struct Snapshot {
    cam_post::Inputs inputs;
    JobHandle<std::map<OperationId, cam::OpState>> statuses;
};

// On the UI thread: the document's snapshot, the library roots and the engine's status job for them.
Result<Snapshot> snapshotOnUi(AppImpl& app, const std::string& documentId, const SetupId& setup, JobContext& ctx)
{
    return app.bridge().call<Snapshot>(
        [&app, documentId, setup]() -> Result<Snapshot> {
            auto entry = app.resolve(documentId);
            if (!entry)
                return tl::unexpected<Error>(entry.error());
            const OpenDocument& od = (*entry)->od;
            Snapshot s;
            s.inputs.setup = setup;
            s.inputs.state = od.document->state();
            s.inputs.build = od.document->build();
            std::string title = od.title;
            if (title.size() > 7 && title.compare(title.size() - 7, 7, ".raycad") == 0)
                title.resize(title.size() - 7);
            s.inputs.documentName = title;
            if (lib::LibraryStack* stack = app.libraries()) {
                for (const lib::LibraryStack::RootInfo& r : stack->roots())
                    s.inputs.libraryRoots.push_back(r.dir);
            }
            cam::ICamEngine* engine = app.cam();
            if (engine == nullptr)
                return tl::unexpected<Error>(Error{ErrorCode::Internal, "The CAM engine is not running", {}});
            s.statuses = engine->statusAsync(s.inputs.state, s.inputs.build);
            if (!s.statuses.valid())
                s.inputs.statuses = engine->status(s.inputs.state, s.inputs.build);
            return s;
        },
        ctx.cancel);
}

Status waitStatuses(Snapshot& s)
{
    if (!s.statuses.valid())
        return std::monostate{};
    auto r = s.statuses.wait();
    if (!r)
        return tl::unexpected(r.error());
    s.inputs.statuses = std::move(*r);
    return std::monostate{};
}

Result<ActionResult> postAction(AppImpl& app, std::string_view argsJson, JobContext& ctx)
{
    auto args = json::parse(argsJson);
    if (!args || !args->isObject())
        return tl::unexpected<Error>(Error{ErrorCode::InvalidArgument, "The arguments must be a JSON object", {}});
    const SetupId setup{Uuid::fromString(args->stringOr("setup", ""))};
    const std::string documentId = args->stringOr("document", "");

    cam_post::Settings settings;
    settings.postId = args->stringOr("post", "");
    settings.directory = args->stringOr("directory", "");
    settings.fileNamePattern = args->stringOr("fileNamePattern", std::string(cam_post::kDefaultFileNamePattern));
    settings.overwrite =
        cam_post::overwriteFromKey(args->stringOr("overwrite", "refuse")).value_or(cam_post::Overwrite::Refuse);
    settings.limitNameLength = args->boolOr("limitNameLength", true);
    if (const Value* n = args->find("programNumber"); n != nullptr && n->isNumber())
        settings.programNumber = static_cast<int>(n->asNumber());
    if (const Value* c = args->find("programComment"); c != nullptr && c->isString())
        settings.programComment = c->asString();
    if (const Value* u = args->find("units"); u != nullptr && u->isString())
        settings.units = unitSystemFromName(u->asString());
    if (const Value* p = args->find("properties"); p != nullptr && p->isObject()) {
        for (const auto& [key, value] : p->asObject())
            settings.properties[key] = valueText(value);
    }

    // 1. The snapshot and its statuses; 2. the gate and the post.
    auto snap = snapshotOnUi(app, documentId, setup, ctx);
    if (!snap)
        return tl::unexpected(snap.error());
    if (auto ok = waitStatuses(*snap); !ok)
        return tl::unexpected(ok.error());
    auto posted = cam_post::run(snap->inputs, settings, ctx);
    if (!posted)
        return tl::unexpected(posted.error());

    // 3. The gate again against the document as it is now, then the file.
    auto now = snapshotOnUi(app, documentId, setup, ctx);
    if (!now)
        return tl::unexpected(now.error());
    if (auto ok = waitStatuses(*now); !ok)
        return tl::unexpected(ok.error());
    Status setupState = std::monostate{};
    if (now->inputs.build) {
        const auto states = cam_setup::setupStates(now->inputs.state, now->inputs.build, ctx);
        if (const auto it = states.find(setup); it != states.end())
            setupState = it->second;
    }
    if (auto ok = cam_post::stillCurrent(*posted, *now->inputs.state, now->inputs.build != nullptr,
                                         now->inputs.statuses, setupState);
        !ok)
        return tl::unexpected(ok.error());
    auto written = cam_post::write(*posted, settings, nullptr, &ctx.cancel);
    if (!written)
        return tl::unexpected(written.error());

    std::size_t lines = 0;
    for (const char c : posted->output.text)
        lines += c == '\n' ? 1 : 0;
    ActionResult result;
    result.resultJson =
        json::canonical(Value::object({{"path", Value::string(written->path)},
                                       {"fileName", Value::string(written->fileName)},
                                       {"replaced", Value::boolean(written->replaced)},
                                       {"lines", Value::integer(static_cast<std::int64_t>(lines))},
                                       {"bytes", Value::integer(static_cast<std::int64_t>(posted->output.text.size()))},
                                       {"hash", Value::string(posted->output.hash().hex())}}));
    result.warnings = posted->output.warnings;
    for (const std::string& note : posted->notes)
        result.warnings.push_back(note);
    return result;
}

} // namespace

Status registerCamPostActions(AppImpl& app)
{
    ActionInfo info;
    info.id = "cam.post";
    info.title = "Post Setup";
    info.category = "CAM";
    info.description = "Posts a setup's toolpaths with a post processor and writes the program file (refused while any "
                       "operation is stale, failed, generating or has a broken reference)";
    info.argsSchemaJson = schema();
    info.changesToolpathsOrGcode = true;
    info.longRunning = true;
    return app.actions().add(std::move(info), [&app](std::string_view argsJson, Caller, JobContext& ctx) {
        return postAction(app, argsJson, ctx);
    });
}

} // namespace ray::api::detail
