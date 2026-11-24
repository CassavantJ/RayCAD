// Setup sheets (ray_api/cam_setupsheet.h, task T-076): the gate, the post job of each setup, the HTML, the file names
// and the file output.
#include "../post/output.h"

#include <ray_api/cam_setup.h>
#include <ray_api/cam_setupsheet.h>

#include <algorithm>
#include <deque>
#include <memory>

namespace ray::api::cam_setupsheet {

namespace {

namespace cp = cam_post;

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

std::string partNamesOf(const doc::DocState& state, const cam::Setup& setup)
{
    std::string out;
    for (const BodyId& id : setup.partBodies) {
        for (const doc::Body& b : state.bodies) {
            if (b.id == id && !b.name.empty())
                out += (out.empty() ? "" : ", ") + b.name;
        }
    }
    return out;
}

// Every occurrence of `from` in `text` replaced by `to`.
std::string replaced(std::string text, std::string_view from, std::string_view to)
{
    for (std::size_t p = text.find(from); p != std::string::npos; p = text.find(from, p + to.size()))
        text.replace(p, from.size(), to);
    return text;
}

// A file error of post/output.h, worded for a setup sheet instead of a program.
Error sheetError(Error e)
{
    e.message = replaced(std::move(e.message), "the program", "the setup sheet");
    e.message = replaced(std::move(e.message), "The program", "The setup sheet");
    return e;
}

post::OverwriteMode modeOf(cp::Overwrite o)
{
    switch (o) {
    case cp::Overwrite::Replace:
        return post::OverwriteMode::Replace;
    case cp::Overwrite::NewName:
        return post::OverwriteMode::NewName;
    case cp::Overwrite::Ask:
    case cp::Overwrite::Refuse:
        return post::OverwriteMode::Refuse;
    }
    return post::OverwriteMode::Refuse;
}

cp::FileName fromPost(const post::FileName& f)
{
    return cp::FileName{f.name, f.stem, f.extension, f.notes};
}

// What one sheet is made of; the job's pointers point into it and into the snapshot.
struct SheetParts {
    cam::Setup setup;
    post::PostJob job;
    std::optional<post::PostInfo> info;
    bool shipped = true;
    const lib::Material* material = nullptr;
    std::string partName;
    cam::CycleTime time;
};

} // namespace

std::vector<View> views(const doc::DocStatePtr& state, const doc::BuildResultPtr& build,
                        const std::vector<SetupId>& setups, JobContext& ctx)
{
    std::vector<View> out;
    for (const SetupId& id : setups) {
        if (!ctx.cancel.check())
            break;
        auto preview = cam_setup::previewSetup(state, build, id, ctx);
        if (!preview)
            continue;
        View v;
        v.setup = id;
        v.origin = preview->origin;
        v.x = preview->x;
        v.y = preview->y;
        v.z = preview->z;
        for (std::size_t i = 0; i < v.corners.size(); ++i)
            v.corners[i] = preview->markers[i];
        out.push_back(v);
    }
    return out;
}

std::vector<cp::Blocker> blockers(const doc::DocState& state, bool built, const std::vector<SetupId>& setups,
                                  const std::map<OperationId, cam::OpState>& statuses,
                                  const std::map<SetupId, Status>& setupStates)
{
    std::vector<cp::Blocker> out;
    for (const SetupId& id : setups) {
        Status own = std::monostate{};
        if (const auto it = setupStates.find(id); it != setupStates.end())
            own = it->second;
        const cam::Setup* setup = findSetup(state, id);
        for (cp::Blocker b : cp::blockers(state, built, id, statuses, own)) {
            if (setups.size() > 1 && setup != nullptr && !(b.op.value == Uuid{}))
                b.name = setup->name + ": " + b.name;
            out.push_back(std::move(b));
        }
    }
    return out;
}

Result<Made> make(const Inputs& in, JobContext& ctx)
{
    if (!in.state || !in.state->cam)
        return tl::unexpected(failure(ErrorCode::NotFound, "The document has no CAM setups"));
    if (in.setups.empty())
        return tl::unexpected(failure(ErrorCode::InvalidArgument, "Choose a setup for the setup sheet"));
    const doc::DocState& state = *in.state;
    const cam::CamState& cam = *state.cam;
    for (const SetupId& id : in.setups) {
        if (findSetup(state, id) == nullptr)
            return tl::unexpected(failure(ErrorCode::NotFound, "The document has no such setup"));
    }
    ctx.progress.set(0.05, "Checking the setups");
    // THE GATE, as for posting: a sheet never describes a program from an operation that is not up to date.
    std::map<SetupId, Status> setupStates;
    if (in.build)
        setupStates = cam_setup::setupStates(in.state, in.build, ctx);
    if (auto ok = ctx.cancel.check(); !ok)
        return tl::unexpected(ok.error());
    const std::vector<cp::Blocker> blocked = blockers(state, in.build != nullptr, in.setups, in.statuses, setupStates);
    if (!blocked.empty()) {
        std::string text = "The setup sheet is blocked:";
        for (const cp::Blocker& b : blocked)
            text += "\n" + (b.name.empty() ? std::string("Setup") : b.name) + ": " + b.message;
        return tl::unexpected(failure(ErrorCode::PostFailed, text));
    }

    Made made;
    std::deque<SheetParts> parts; // stable addresses: the jobs point into their own part
    for (const SetupId& id : in.setups) {
        if (auto ok = ctx.cancel.check(); !ok)
            return tl::unexpected(ok.error());
        const cam::Setup& setup = *findSetup(state, id);
        SheetParts& p = parts.emplace_back();
        p.setup = setup;
        const lib::MachineDef* machine = cp::machineOf(state, id);
        // What posting the setup would use now (cam_post.h "Remembered per setup").
        const std::optional<cp::Settings> remembered = cp::rememberedSettings(in.userDataDir, id);
        std::string postId = remembered ? remembered->postId : std::string();
        if (postId.empty())
            postId = setup.postId;
        if (postId.empty() && machine != nullptr)
            postId = machine->defaultPost;
        if (!postId.empty()) {
            const std::string script = cp::findPost(in.libraryRoots, postId);
            if (script.empty()) {
                made.notes.push_back(setup.name + ": the post '" + postId + "' is not in the library");
                p.setup.postId = postId;
            } else if (auto info = cp::inspectPost(script)) {
                p.info = std::move(*info);
                const std::string shipped = cp::findPost(in.shippedRoots, postId);
                p.shipped = !shipped.empty() && shipped == script;
            } else {
                made.notes.push_back(setup.name + ": the post '" + postId +
                                     "' could not be read: " + info.error().message);
                p.setup.postId = postId;
            }
        }
        if (const auto m = cam.materials.find(setup.material); m != cam.materials.end())
            p.material = &m->second.item;
        p.partName = partNamesOf(state, setup);

        p.job.setup = &p.setup;
        p.job.machine = machine;
        p.job.outputUnits = remembered && remembered->units ? *remembered->units : state.settings.units;
        p.job.documentName = in.documentName;
        p.job.fixedDate = in.date;
        std::vector<cam::CycleTimeOperation> timed;
        for (const OperationId& opId : setup.operations) {
            const auto op = cam.operations.find(opId);
            if (op == cam.operations.end() || op->second.suppressed)
                continue;
            const auto st = in.statuses.find(opId);
            if (st == in.statuses.end() || st->second.status == cam::OpStatus::Suppressed || !st->second.toolpath)
                continue;
            const auto tool = cam.tools.find(op->second.tool.tool);
            const auto holder = cam.holders.find(op->second.tool.holder);
            p.job.sections.push_back(post::PostSection{&op->second, st->second.toolpath.get(),
                                                       tool == cam.tools.end() ? nullptr : &tool->second.item,
                                                       holder == cam.holders.end() ? nullptr : &holder->second.item});
            timed.push_back(cam::CycleTimeOperation{st->second.toolpath.get(), op->second.tool.toolNumber});
        }
        // The estimate the CAM tree shows (cam_cycletime::estimate: the default options), with a machine that knows
        // nothing when the setup has none (every default applies; the gate refuses that case anyway).
        p.time = cam::estimateCycleTime(timed, machine != nullptr ? *machine : lib::MachineDef{});
    }
    ctx.progress.set(0.6, "Writing the setup sheet");
    std::vector<post::SetupSheetInput> inputs;
    for (const SheetParts& p : parts) {
        post::SetupSheetInput s;
        s.job = &p.job;
        s.time = p.time;
        if (const auto pic = in.pictures.find(p.setup.id); pic != in.pictures.end())
            s.fixtureImagePng = pic->second;
        s.post = p.info ? &*p.info : nullptr;
        s.postShipped = p.shipped;
        s.material = p.material;
        s.partName = p.partName;
        inputs.push_back(std::move(s));
    }
    // The stock boxes: the setups resolve (the gate says so).
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        auto preview = cam_setup::previewSetup(in.state, in.build, in.setups[i], ctx);
        if (!preview)
            return tl::unexpected(preview.error());
        inputs[i].stock = preview->resolved.stockBounds;
    }
    auto html = post::renderSetupSheetsHtml(inputs);
    if (!html)
        return tl::unexpected(html.error());
    made.html = std::move(*html);
    ctx.progress.set(1.0, "Setup sheet ready");
    return made;
}

Result<cp::FileName> fileNameFor(const doc::DocState& state, const std::vector<SetupId>& setups,
                                 std::string_view documentName, std::string_view extension,
                                 const std::string& userDataDir)
{
    if (setups.empty())
        return tl::unexpected(failure(ErrorCode::InvalidArgument, "Choose a setup for the setup sheet"));
    const cam::Setup* setup = findSetup(state, setups.front());
    if (setup == nullptr)
        return tl::unexpected(failure(ErrorCode::NotFound, "The document has no such setup"));
    post::NameFields f;
    f.programNumber = setup->programNumber;
    f.programComment = setup->programComment;
    f.setupName = setup->name;
    f.documentName = std::string(documentName);
    f.partName = partNamesOf(state, *setup);
    if (f.partName.empty())
        f.partName = f.documentName;
    f.postId = setup->postId;
    if (const lib::MachineDef* m = cp::machineOf(state, setups.front()))
        f.machineName = m->name;
    f.extension = std::string(extension);
    std::string pattern = std::string(cp::kDefaultFileNamePattern);
    if (setups.size() > 1) {
        pattern = "{doc}_setups";
    } else if (const auto r = cp::rememberedSettings(userDataDir, setups.front()); r && !r->fileNamePattern.empty()) {
        pattern = r->fileNamePattern;
        if (!r->postId.empty())
            f.postId = r->postId;
    }
    auto name = post::fileNameFor(pattern, f, 0);
    if (!name && setups.size() == 1) // a remembered pattern that no longer reads: the default
        name = post::fileNameFor(cp::kDefaultFileNamePattern, f, 0);
    if (!name)
        return tl::unexpected(name.error());
    return fromPost(*name);
}

cp::FileName fileNameFromStem(std::string_view stem, std::string_view extension)
{
    bool changed = false;
    const std::string safe = post::sanitizeFileName(stem, &changed);
    cp::FileName f;
    f.stem = safe;
    f.extension = std::string(extension);
    f.name = safe + f.extension;
    if (changed)
        f.notes.push_back("Characters a file name cannot hold were replaced: " + f.name);
    return f;
}

std::string rememberedFolder(const std::string& userDataDir, const SetupId& setup)
{
    const auto r = cp::rememberedSettings(userDataDir, setup);
    return r ? r->directory : std::string();
}

Result<cp::Written> write(std::string_view bytes, const std::string& directory, const std::string& fileName,
                          cp::Overwrite overwrite, post::IOutputFs* fs, CancelToken* cancel)
{
    if (directory.empty())
        return tl::unexpected(post::outputError(ErrorCode::InvalidArgument, post::OutputProblem::FolderMissing,
                                                "Choose the folder to save the setup sheet in"));
    post::OutputTarget t;
    t.directory = directory;
    t.fileName = fileName;
    t.mode = modeOf(overwrite);
    auto written = post::writeProgramFile(bytes, t, fs != nullptr ? *fs : post::localOutputFs(), cancel);
    if (!written)
        return tl::unexpected(sheetError(written.error()));
    return cp::Written{written->path, written->fileName, written->replaced};
}

Result<bool> exists(const std::string& directory, const std::string& fileName, post::IOutputFs* fs)
{
    if (directory.empty())
        return tl::unexpected(post::outputError(ErrorCode::InvalidArgument, post::OutputProblem::FolderMissing,
                                                "Choose the folder to save the setup sheet in"));
    post::OutputTarget t;
    t.directory = directory;
    t.fileName = fileName;
    auto there = post::targetExists(t, fs != nullptr ? *fs : post::localOutputFs());
    if (!there)
        return tl::unexpected(sheetError(there.error()));
    return *there;
}

} // namespace ray::api::cam_setupsheet
