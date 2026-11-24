// What the desktop CAM workspace (src/ui/cam, task T-044) needs from cam, doc and lib beyond the cam.* actions and
// IApp::cam(), without Qt: the live setup preview (cam::resolveSetup on a document snapshot, with the stock as a solid
// in the model frame and the origin markers), each setup's own resolution for the CAM tree, the JSON of a library
// machine for a command's "library" member, and the post scripts of the library roots.
//
// src/ui may link only api and render (PLAN.md 1.1), so it never calls cam, doc or lib itself. Documents change only
// through the actions (cam.setup.*, cam.operation.*); these helpers only read. Not one of the contract headers in
// include/ray/.
#pragma once

#include <ray/api.h>

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ray::api::cam_setup {

// T-074: the stocks that setups whose stock comes from an earlier setup (cam::FromSetupStock) start from, by setup id:
// what ICamEngine::carriedStock gave for them (the UI asks it, as a job with progress, before it resolves such a
// setup).
using CarriedStocks = std::map<SetupId, std::shared_ptr<const cam::CarriedStock>>;
// The setups of `state` whose stock comes from an earlier setup, in program order.
std::vector<SetupId> carriedSetups(const doc::DocState& state);

// The setup preview: what the setup panel draws.
struct Preview {
    cam::ResolvedSetup resolved;   // frame (both ways), part and stock bounds in the WCS; resolved.stockSolid is null
    kernel::Shape stock;           // the stock solid in the model frame (a box or cylinder made in the WCS and moved
                                   // back, or the solid stock body itself); null for a carried stock (stockMesh)
    std::array<Vec3, 9> markers{}; // the stock box's corners 0..7 (cam.h numbering) and its top centre, model frame
    Vec3 origin{}, x{}, y{}, z{};  // the WCS origin and unit axes, model frame
    // T-074: a stock carried from an earlier setup (FromSetupStock) as a display mesh in the model frame (its triangles
    // flat-shaded, one face range); null for the other stock types, which `stock` holds.
    std::shared_ptr<const kernel::Mesh> stockMesh;
};
// Setup `setup` of `state` as `build` resolves it (cam::resolveSetup over doc::camSnapshot), for display. The errors of
// resolveSetup (a Z face that is not flat, a reference that does not resolve, ...), NotFound when the state has no such
// setup, KernelFailure when the stock solid cannot be made. Runs kernel queries: call it on a worker.
// T-074: `carried` is the stock a FromSetupStock setup starts from (ICamEngine::carriedStock for this state and setup);
// such a setup without it is resolveSetup's InvalidArgument (the earlier setup must be simulated first).
Result<Preview> previewSetup(const doc::DocStatePtr& state, const doc::BuildResultPtr& build, const SetupId& setup,
                             JobContext& ctx, const cam::CarriedStock* carried = nullptr);

// Every setup's own resolution (the frame and stock, no operation): an empty Status when it resolves, else the error
// resolveSetup gives (ReferenceBroken for a WCS reference or a body that does not resolve). Nothing without a build.
// Call it on a worker; Cancelled stops it early (the setups not reached are left out).
// T-074: a setup whose stock comes from an earlier setup is resolved from its entry in `carried` when there is one (the
// whole setup, its carried stock included). Without one, everything of it but the stock is checked (the frame, its
// references, the origin: the setup resolved with a box stock around its part bodies in place of the carried one),
// because the carried stock is known only once the earlier setup is simulated; its operations' input hashes carry the
// earlier setup's (cam.h operationInputHash), so a stale chain still shows as stale operations.
std::map<SetupId, Status> setupStates(const doc::DocStatePtr& state, const doc::BuildResultPtr& build, JobContext& ctx,
                                      const CarriedStocks* carried = nullptr);

// The JSON of a library machine as a cam.* command's "library" member takes it (the library's file format).
std::string machineJson(const lib::MachineDef& machine);
// The JSON of a stored reference as the cam.* commands take it (naming::toJson), e.g. a setup's Z face to edit it.
std::string referenceJson(const naming::TopoRef& ref);

// The post scripts (file stems of posts/*.post.js) in the given library root folders, sorted, without duplicates.
// Folders that cannot be read are skipped. Reads folders, which may be network shares: call it on a worker.
std::vector<std::string> postStems(const std::vector<std::string>& rootDirs);

} // namespace ray::api::cam_setup
