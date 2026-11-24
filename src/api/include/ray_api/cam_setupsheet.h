// Setup sheets (task T-076): what the desktop's setup sheet dialog (src/ui/cam/setupsheet*) needs from cam, doc, lib
// and post, without Qt. A sibling of ray_api/cam_post.h; not one of the contract headers in include/ray/.
//
// A sheet describes the program the post dialog would write for the setup, so it is made under the same gate
// (cam_post::blockers): nothing stale, failed, generating or without a toolpath, a tool or a machine. The post, the
// units and the program number are what posting the setup would use now: the post and units last used for it
// (cam_post::rememberedSettings), else the setup's post or its machine's default and the document's units; the
// setup's own program number and comment. The cycle time is cam::estimateCycleTime of the posted operations (the
// estimate the CAM tree shows). The HTML is post::renderSetupSheetsHtml; the desktop makes the PDF from it.
//
// The steps, because the engine's status and the library stack live on the UI thread:
//   1. on the UI thread: the document's snapshot and build, ICamEngine::statusAsync's statuses, the library roots;
//   2. on a worker: views() (each setup's frame and stock, for the pictures);
//   3. on the UI thread: the pictures (IViewport::captureImage);
//   4. on a worker: make() -> the HTML; then the PDF (src/ui) and write().
#pragma once

#include <ray/api.h>
#include <ray_api/cam_post.h>

#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ray::api::cam_setupsheet {

// One setup's frame and stock as the picture needs them (model frame): the WCS origin and unit axes, and the stock
// box's corners (cam.h numbering: 0..3 the top, from the front left, anticlockwise seen from +Z).
struct View {
    SetupId setup;
    Vec3 origin{}, x{}, y{}, z{};
    std::array<Vec3, 8> corners{};
};
// cam_setup::previewSetup of each setup, in the order given; a setup that does not resolve is left out (the gate
// names it). Kernel queries: a worker.
std::vector<View> views(const doc::DocStatePtr& state, const doc::BuildResultPtr& build,
                        const std::vector<SetupId>& setups, JobContext& ctx);

struct Inputs {
    std::vector<SetupId> setups; // one sheet each, in this order (the document's program order for "all setups")
    doc::DocStatePtr state;
    doc::BuildResultPtr build;
    std::map<OperationId, cam::OpState> statuses;       // ICamEngine::status(Async) of state and build
    std::vector<std::string> libraryRoots;              // LibraryStack::roots, lowest first
    std::vector<std::string> shippedRoots;              // the library shipped with RayCAD (its posts get the notice)
    std::string documentName;                           // the document's title without ".raycad"
    std::string userDataDir;                            // the remembered post settings ("" for none)
    std::map<SetupId, std::vector<std::byte>> pictures; // PNG per setup; a setup without one gets "No picture"
    std::optional<std::string> date;                    // ISO date printed on the sheet (none when empty)
};
struct Made {
    std::string html;
    std::vector<std::string> notes; // what was assumed: a post that is not in the library, ...
};
// The gate for every setup (ErrorCode::PostFailed: "Setup sheet blocked:" and one line per blocker, each line naming
// the setup), then the sheets. NotFound for a setup the document does not have. A worker.
Result<Made> make(const Inputs& in, JobContext& ctx);

// What blocks a sheet of `setups` (cam_post::blockers of each; `setupStates` from cam_setup::setupStates, a setup
// missing from it counts as resolving). Each blocker's name is "<setup>: <operation>" when there are several setups.
std::vector<cam_post::Blocker> blockers(const doc::DocState& state, bool built, const std::vector<SetupId>& setups,
                                        const std::map<OperationId, cam::OpState>& statuses,
                                        const std::map<SetupId, Status>& setupStates);

// The default file name, with `extension` (".pdf", ".html"): for one setup the program file's name (the naming pattern
// last used to post it, else cam_post::kDefaultFileNamePattern, with no length limit: no control reads it), for
// several "<doc>_setups". Made safe as program names are (post/output.h).
Result<cam_post::FileName> fileNameFor(const doc::DocState& state, const std::vector<SetupId>& setups,
                                       std::string_view documentName, std::string_view extension,
                                       const std::string& userDataDir);
// The stem the user typed, made safe as program names are, with the extension.
cam_post::FileName fileNameFromStem(std::string_view stem, std::string_view extension);
// The folder the program of the first setup was last written to ("" when none).
std::string rememberedFolder(const std::string& userDataDir, const SetupId& setup);

// Writes `bytes` as `fileName` in `directory`, atomically, with the overwrite rule (Ask behaves as Refuse: the caller
// asks first), through post/output.h as programs are written; errors as cam_post::write's (cam_post::problemKey reads
// them), worded for a setup sheet. `fs` null: the real file system. A worker.
Result<cam_post::Written> write(std::string_view bytes, const std::string& directory, const std::string& fileName,
                                cam_post::Overwrite overwrite, post::IOutputFs* fs = nullptr,
                                CancelToken* cancel = nullptr);
// Whether that file exists now (for Ask). A worker.
Result<bool> exists(const std::string& directory, const std::string& fileName, post::IOutputFs* fs = nullptr);

} // namespace ray::api::cam_setupsheet
