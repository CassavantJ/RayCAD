// What the operation panel of the CAM workspace (src/ui/cam/operation*, task T-045) needs from cam, doc, lib and sim
// beyond the cam.* actions and IApp::cam(), without Qt: the registered operation types and their schemas, the
// arguments OK would run (so a preview applies exactly what the action will), numeric fields as the document's
// expressions read them, library items as the commands' "library" member takes them, the feeds calculator, the holes a
// hole picker offers, and the backplot of a toolpath in the model frame.
//
// src/ui may link only api and render (PLAN.md 1.1), so it never calls cam, doc, lib or sim itself. Documents change
// only through the actions (cam.operation.*, cam.setup.*); these helpers only read. Not one of the contract headers
// in include/ray/. A sibling of ray_api/cam_setup.h (T-044).
#pragma once

#include <ray/api.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ray::api::cam_operation {

// ---------- Operation types ----------
struct TypeInfo {
    std::string name;       // IOperationType::typeName ("drill")
    std::string title;      // the schema's "title" ("Drilling"), else the name
    std::string schemaJson; // IOperationType::paramsSchemaJson
};
// Every registered type, sorted by name (OperationRegistry::types). Empty without a registry.
std::vector<TypeInfo> operationTypes(IApp& app);

// cam.operation.add / cam.operation.edit as the action runs them on `state`: "params" checked against the type's
// schema and completed with its defaults (the API's check), so Document::beginPreview previews exactly what OK
// commits. The other members are checked by the command itself when it is applied. Errors name the JSON Pointer of the
// bad value ("/params/peck: must be at least 0").
Result<doc::Command> completeCommand(IApp& app, const doc::DocState& state, const doc::Command& command);

// ---------- Numeric fields ----------
// The value of a field as the document's expressions read it (doc::evaluate over the document's parameters): Length
// in mm, Angle in degrees (the expression may say deg or rad), Count and Unitless as plain numbers. Errors say what is
// wrong with the expression.
Result<double> evaluate(const doc::DocState& state, std::string_view expression, doc::Parameter::Kind kind);

// ---------- Library items ----------
// The JSON of an item as a cam.* command's "library" member takes it (the library's file format, lib::toJson).
std::string toolJson(const lib::Tool& tool);
std::string holderJson(const lib::Holder& holder);
std::string materialJson(const lib::Material& material);
// The tool type as the library files spell it ("flatEndMill", "spotDrill", "drill", "tap", ...) and back.
std::string toolTypeName(lib::ToolType type);
std::optional<lib::ToolType> toolTypeFromName(std::string_view name);

// ---------- Feeds and speeds ----------
// cam::computeFeeds for an operation of type `opType` (pure; no document involved). `opClass` "" lets it choose.
struct FeedsRequest {
    lib::Tool tool;
    lib::Material material;
    lib::MachineDef machine;
    std::string opType;
    std::string opClass;
    std::optional<double> surfaceSpeedMMin, chipLoadMm;
    double radialEngagementMm = 0, axialDepthMm = 0;
};
Result<cam::FeedsResult> computeFeeds(const FeedsRequest& request);

// ---------- Holes (x-geometry kind "hole") ----------
// One hole of a setup's part bodies as recognised along the setup's Z (cam::recognizeHoles with the thread pickup of
// the hole features), for a picker: which faces belong to it, which one is its bore wall (the face the operation
// stores), and what it is.
struct Hole {
    BodyId body;
    std::uint32_t boreFace = 0;       // RecognizedHole::axisSub
    std::vector<std::uint32_t> faces; // every face of the hole, sorted
    double diameter = 0, depth = 0;   // the bore, mm (depth: top to lowest point)
    bool through = false;
    std::string threadSpec; // the hole feature's thread, "" when not tapped or not known
    Vec3 top{};             // the hole's top centre, model frame
    double x = 0, y = 0;    // the axis in the WCS, mm
};
// Every hole of setup `setup`'s part bodies in `state` as `build` resolves it. The errors of resolveSetup and
// recognizeHoles (ReferenceBroken, KernelFailure, Cancelled); NotFound when the state has no such setup. Kernel work:
// call it on a worker.
Result<std::vector<Hole>> holesOf(const doc::DocStatePtr& state, const doc::BuildResultPtr& build, const SetupId& setup,
                                  JobContext& ctx);

// ---------- Display ----------
// The backplot of `toolpath` in the model frame of setup `setup` (sim::buildBackplot with the setup's modelFromWcs and
// its embedded machine, so cycles expand as the post would), arcs within 0.01 mm. Call it on a worker.
Result<std::shared_ptr<const sim::Backplot>> backplot(const doc::DocStatePtr& state, const doc::BuildResultPtr& build,
                                                      const SetupId& setup, const cam::Toolpath& toolpath,
                                                      JobContext& ctx);

} // namespace ray::api::cam_operation
