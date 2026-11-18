// What the desktop viewer (src/ui, task T-020) needs from the modules below the API, without Qt: importing a file
// for display, display meshes, the properties of a picked element, persistent references for a selection, and the
// job system while there is no application session.
//
// src/ui may link only api and render (PLAN.md 1.1), so it never calls kernel or naming itself; everything it needs
// from them goes through here. Every function that can take more than a few milliseconds takes a JobContext and
// is meant to run on a worker. Not one of the contract headers in include/ray/.
//
// Seams for later tasks:
//   - T-019 replaced the UI's use of importModel with IApp::open; bodiesFromDocument turns the document's build
//     into the Body list. importModel stays for tests and tools that want a file's bodies without a document.
//   - T-022 builds the real measure tools; describeElement is the simple read-out of T-020.
#pragma once

#include <ray/api.h>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ray::api::viewer {

// One body to show: shape plus the names of its faces, edges and vertices when the source has them.
struct Body {
    BodyId id;
    std::string name;
    kernel::Shape shape;
    std::optional<naming::ElementMap> map;
    std::array<float, 4> color{0.75f, 0.75f, 0.75f, 1.0f}; // linear RGBA from the source file (display hint only)
    bool visible = true;
};

// ---------- Opening a file without a document (tests and tools) ----------
// True for .step, .stp, .igs and .iges (any case).
bool isImportable(std::string_view path);
struct ImportedModel {
    std::vector<Body> bodies;
    UnitSystem sourceUnits = UnitSystem::Metric;
    std::vector<std::string> warnings;
};
// Reads a STEP or IGES file and names every body with naming::nameImported under one new import feature id.
// Progress stages come from the kernel ("Reading file", "Transferring", "Healing", ...) and then "Naming".
// The kernel's model-load stage cannot be cancelled (PLAN.md 7): the caller abandons the job instead.
Result<ImportedModel> importModel(const std::string& utf8Path, JobContext&);

// ---------- From a document build (T-017/T-019 feed the viewer through this) ----------
// One Body per body of `state` that the build produced, in the state's body order, with the build's element maps.
std::vector<Body> bodiesFromDocument(const doc::DocState& state, const doc::BuildResult& build);
struct FeatureRow {
    FeatureId id;
    std::string name, type;
    bool suppressed = false;
    std::optional<doc::FeatureStatus> status; // from the build's reports, when there is one
};
std::vector<FeatureRow> featuresOf(const doc::DocState& state, const doc::BuildResult* build);

// ---------- Display meshes ----------
// The first view of an imported model is meshed at 0.2 mm / 20 degrees on every tier (PLAN.md 9).
constexpr double kCoarseChordMm = 0.2;
constexpr double kCoarseAngleDeg = 20.0;
// Meshes one body for display. Angles in degrees.
Result<kernel::Mesh> meshBody(const kernel::Shape&, double chordMm, double angleDeg, JobContext&);

// ---------- Selection ----------
// The kernel vertex a renderer vertex hit names: of the two ends of edge `edgeIndex`, the one at `point` (within
// 1e-3 of the edge length scale). nullopt if the edge has no vertex there.
std::optional<kernel::SubId> vertexOfEdgeEnd(const kernel::Shape&, std::uint32_t edgeIndex, Vec3 point);

// Persistent references over a set of bodies (those that have element maps).
class SelectionResolver {
public:
    explicit SelectionResolver(std::span<const Body> bodies);
    ~SelectionResolver();
    SelectionResolver(const SelectionResolver&) = delete;
    SelectionResolver& operator=(const SelectionResolver&) = delete;

    // nullopt when the body has no element map (then a selection is held by SubId only).
    std::optional<naming::TopoRef> makeRef(BodyId, kernel::SubId) const;
    naming::Resolved resolve(const naming::TopoRef&) const;

private:
    std::vector<BodyId> named_; // bodies that have a map, sorted
    std::unique_ptr<naming::IResolver> resolver_;
};

// ---------- Properties read-out (T-022 builds the real measure tools) ----------
struct ElementInfo {
    kernel::SubKind kind = kernel::SubKind::Solid;
    // Face
    std::optional<kernel::SurfaceInfo> surface;
    // Edge
    std::optional<kernel::CurveInfo> curve;
    // Vertex
    std::optional<Vec3> point;
    // Body (kind Solid)
    std::optional<kernel::MassProps> mass;
    std::optional<Box3> bounds;
};
// Runs kernel queries: call on a worker. A Solid SubId (any index) describes the whole body.
Result<ElementInfo> describeElement(const kernel::Shape&, kernel::SubId, JobContext&);
// Short English names for the read-out ("Plane", "Cylinder", "Line", "Circle", ...).
std::string_view surfaceTypeName(kernel::SurfaceType);
std::string_view curveTypeName(kernel::CurveType);

// ---------- Jobs while there is no application session ----------
// IApp::jobs() replaces this once T-017's session exists. One job system per process (it sets oneTBB's limit).
std::unique_ptr<IJobSystem> makeViewerJobs(IUiDispatcher& dispatcher, int workerCount = 0);

} // namespace ray::api::viewer
