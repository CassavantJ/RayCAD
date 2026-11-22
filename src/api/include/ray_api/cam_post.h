// Posting a setup (task T-047): what the desktop's post dialog and G-code viewer (src/ui/cam/post*) and the cam.post
// action need from cam, doc, lib and post, without Qt. A sibling of ray_api/cam_setup.h and cam_operation.h; not one
// of the contract headers in include/ray/.
//
// SAFETY. Posting is refused, with every reason named, while any operation of the setup that is not suppressed is
// stale, failed, still generating, has a broken reference, has no toolpath or no tool, or the setup itself does not
// resolve or has no machine (blockers()). This is the ONE gate: run() applies it before the post engine sees anything,
// stillCurrent() applies it again (against the document as it is then) just before write() puts a file in place, and
// both the dialog and the cam.post action go through these three, so no path writes a program from an operation that
// is not up to date. The post engine itself refuses a section without a toolpath (post.h).
//
// The steps, because the engine's status and the library stack live on the UI thread:
//   1. on the UI thread: Inputs (the document's snapshot, its build, ICamEngine::statusAsync's statuses for them, the
//      library roots, the document's name);
//   2. on a worker: run() -> Posted (the gate, then IPostEngine::run with a fresh engine of its own);
//   3. on the UI thread: the statuses again; on a worker: stillCurrent(), then write() (post/output.h: the naming
//      pattern, the overwrite rule, the atomic write; the bytes are exactly PostOutput::text).
// Remembered settings (rememberedSettings / remember): per setup, in <user data dir>/post-settings.json, as the setup
// has no member for post properties or an output folder (cam.h); the post id, program number and comment are the
// setup's own (cam.setup.edit).
#pragma once

#include <ray/api.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ray::post {
class IOutputFs; // src/post/output.h: the file system seam (tests inject fakes)
}

namespace ray::api::cam_post {

inline constexpr std::string_view kDefaultFileNamePattern = "{doc}_{setup}"; // post/output.h
struct FileName {
    std::string name, stem, extension; // extension with its dot
    std::vector<std::string> notes;    // what was changed to make the name safe or short enough
};
struct Written {
    std::string path, fileName;
    bool replaced = false;
};
// What kind of file problem an error of write() is: "exists", "locked", "folderMissing", "unreachable", "diskFull",
// "denied", "cancelled", "other", or "" when it is not a file problem.
std::string problemKey(const Error& error);

// ---------- The gate ----------
enum class BlockReason {
    NotBuilt,        // the model has not been built
    NoSetup,         // the document has no such setup
    SetupBroken,     // the setup's WCS or stock does not resolve
    NoMachine,       // the setup names no machine the document holds
    NoOperations,    // nothing to post (none, or every one suppressed)
    Stale,           // the toolpath is older than the operation's inputs
    Generating,      // a generation is running
    Failed,          // the last generation failed
    ReferenceBroken, // a geometry reference does not resolve
    NoToolpath,      // never generated
    NoTool,          // the operation's tool is not in the document
};
std::string_view reasonKey(BlockReason reason); // "stale", "referenceBroken", ...
struct Blocker {
    OperationId op;         // nil when it is about the setup
    std::string name;       // the operation's name (the setup's for a setup reason)
    BlockReason reason;
    std::string message;    // what is wrong, in words (with the engine's message where it has one)
    bool generateFixes = false; // generating the operation is the fix (Stale, NoToolpath)
};
// Why setup `setup` of `state` cannot be posted now; empty when it can. `statuses`: ICamEngine::status of `state`
// with the build `built` says exists (any operation missing from it counts as never generated); `setupState`: the
// setup's own resolution (cam_setup::setupStates), an empty Status when it resolves. Pure; any thread.
std::vector<Blocker> blockers(const doc::DocState& state, bool built, const SetupId& setup,
                              const std::map<OperationId, cam::OpState>& statuses, const Status& setupState);
// One line per blocker: "Drill 1: stale: ...".
std::string blockersText(const std::vector<Blocker>& list);

// ---------- Posts and their properties ----------
// The script of post `id` in the library roots (lowest first; a higher root wins), "" when none has it. Reads
// folders: a worker.
std::string findPost(const std::vector<std::string>& libraryRootsLowestFirst, std::string_view id);
// IPostEngine::inspect with an engine of its own (each call; thread-safe).
Result<post::PostInfo> inspectPost(const std::string& scriptPath);
// The post's declared properties as a JSON Schema for the schema form (ui SchemaFormModel): one property per
// declaration in declaration order (x-propertyOrder), bool -> boolean, int -> integer, number -> number, enum ->
// string with enum, string -> string; title, description (with the group in front), and as the default the
// machine's value (MachineDef::postPropertyOverrides) where it has one, else the post's.
std::string propertiesSchemaJson(const post::PostInfo& info, const lib::MachineDef* machine);
// The values of the schema form (a JSON object, as SchemaFormModel::params gives it) as PostJob::properties: only
// those that differ from the default above, in the text the post host reads ("true", "8", "0.25", "every").
// Unknown keys are dropped.
std::map<std::string, std::string> propertyOverrides(const post::PostInfo& info, const lib::MachineDef* machine,
                                                     std::string_view valuesJson);
// The other way: PostJob::properties as a JSON object of typed values for SchemaFormModel::load.
std::string propertyValuesJson(const post::PostInfo& info, const std::map<std::string, std::string>& overrides);

// ---------- Settings of one post run ----------
enum class Overwrite { Ask, Replace, NewName, Refuse };
std::string_view overwriteKey(Overwrite o); // "ask", "replace", "newName", "refuse"
std::optional<Overwrite> overwriteFromKey(std::string_view key);
struct Settings {
    std::string postId;                            // "" = the setup's post, else the machine's default
    std::map<std::string, std::string> properties; // overrides (propertyOverrides)
    std::optional<int> programNumber;              // the setup's when unset
    std::optional<std::string> programComment;     // the setup's when unset
    std::optional<UnitSystem> units;               // the document's when unset
    std::string directory;                         // the output folder
    std::string fileNamePattern = std::string(kDefaultFileNamePattern);
    Overwrite overwrite = Overwrite::Ask;          // Ask behaves as Refuse in write(): the caller asks first
    bool limitNameLength = true;                   // cut the stem to the machine's maxProgramNameLength
    std::optional<std::string> date;               // ISO date for the includeDate property (else no date)
};

// ---------- Posting ----------
struct Inputs {
    SetupId setup;                                // the setup to post
    doc::DocStatePtr state;
    doc::BuildResultPtr build;
    std::map<OperationId, cam::OpState> statuses; // ICamEngine::status(Async) of state and build
    std::vector<std::string> libraryRoots;        // LibraryStack::roots, lowest first
    std::string documentName;                     // the document's title without ".raycad"
};
struct Posted {
    doc::DocStatePtr basis; // the snapshot that was posted
    post::PostOutput output;
    post::PostInfo info;
    std::string scriptPath;
    SetupId setup;
    std::string setupName, machineName;
    bool machineVerified = false;
    std::vector<OperationId> sectionOps;      // PostOutput::lineMap's sectionIndex -> operation
    std::vector<std::string> sectionNames;
    std::vector<Hash256> sectionInputs;       // each posted toolpath's input hash (stillCurrent compares them)
    std::vector<std::string> notes;           // operations left out (suppressed), name changes
    FileName fileName;                        // the pattern applied
    int maxStemLength = 0;                    // the stem limit applied (0: none)
};
// The file name `settings` give for setup `setup` of `state` with post `info` (for the dialog's preview).
Result<FileName> fileNameFor(const doc::DocState& state, const SetupId& setup, const post::PostInfo& info,
                             const Settings& settings, std::string_view documentName);
// The gate (blocked: ErrorCode::PostFailed, the message from blockersText), then the post. Errors: NotFound for a
// missing setup or post script, InvalidArgument for a bad pattern or property, the post engine's (ScriptError naming
// the script line for a post that refuses its input), Cancelled. A worker.
Result<Posted> run(const Inputs& in, const Settings& settings, JobContext& ctx);
// Each operation `posted` posted is still in the setup, in the same order, with the same toolpath input hash and
// UpToDate in `nowStatuses`, and the gate still passes for the document `now` (built `built`): else PostFailed naming
// what changed. Any thread.
Status stillCurrent(const Posted& posted, const doc::DocState& now, bool built,
                    const std::map<OperationId, cam::OpState>& nowStatuses, const Status& setupState);
// The file: Settings::directory, the name worked out by run(), Settings::overwrite (Ask = Refuse). `fs` null: the real
// file system. A worker.
Result<Written> write(const Posted& posted, const Settings& settings, post::IOutputFs* fs = nullptr,
                      CancelToken* cancel = nullptr);
// Whether the file write() would make exists now (for Ask). A worker (the folder may be a slow share).
Result<bool> targetExists(const Posted& posted, const Settings& settings, post::IOutputFs* fs = nullptr);
// The machine of a setup as the document embeds it; null when it has none.
const lib::MachineDef* machineOf(const doc::DocState& state, const SetupId& setup);

// ---------- Remembered per setup ----------
// What was last used for `setup` (post id, properties, units, folder, pattern, overwrite rule, name limit; not the
// program number and comment, which the setup holds), from <userDataDir>/post-settings.json; when the setup has none,
// the folder, pattern and overwrite rule last used for any setup; nullopt when nothing is stored or userDataDir is "".
std::optional<Settings> rememberedSettings(const std::string& userDataDir, const SetupId& setup);
// Stores them (an atomic replace of the file). IoError when it cannot be written.
Status remember(const std::string& userDataDir, const SetupId& setup, const Settings& settings);

} // namespace ray::api::cam_post
