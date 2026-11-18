// RayCAD interface contract: shared libraries (tools, holders, materials, machines, templates, shop rules)
// Module: src/lib   Depends on: core
// Status: PLAN DRAFT. JSON schemas for every file type are in PLAN.md section 3.6 and schemas/*.schema.json.
//
// Storage: plain folders, one canonical-JSON file per item, so a shop can share a library on a network
// drive and two people rarely touch the same file. A document EMBEDS a copy of every library item it
// uses (tool, holder, machine, material), so a .raycad file is self-contained and reproducible.
//
//   <library root>/
//     tools/<uuid>.tool.json          holders/<uuid>.holder.json      materials/<uuid>.material.json
//     machines/<uuid>.machine.json    posts/<name>.post.js            templates/<uuid>.optemplate.json
//     rules/<uuid>.rule.json (v2)     library.json (name, format version)
#pragma once
#include "core.h"
#include <map>
#include <optional>

namespace ray::lib {

enum class ToolType { FlatEndMill, BallEndMill, BullEndMill, ChamferMill, FaceMill, SpotDrill, Drill, Tap,
                      Reamer, BoringBar, ThreadMill, EngraveCutter };

struct ToolPreset {              // per-material cutting data
    MaterialId material;
    std::string opClass;         // "rough","finish","slot","drill","tap"
    double surfaceSpeedMMin, chipLoadMm, maxRadialFraction, maxAxialFraction, rampAngleDeg;
};
struct Tool {
    ToolId id;
    std::string name, vendor, productId;
    ToolType type;
    int flutes;
    double diameter, shankDiameter, fluteLength, shoulderLength, overallLength;
    double cornerRadius = 0, tipAngleDeg = 0, tipDiameter = 0;   // bull / drill, chamfer, spot
    double threadPitch = 0; std::string threadSpec;              // taps and thread mills, e.g. "1/4-20 UNC", "M6x1"
    std::string bodyMaterial;                                    // "carbide","HSS"
    std::vector<ToolPreset> presets;
    int defaultToolNumber = 0;
    Hash256 contentHash() const;
};
struct HolderSegment { double height, lowerDiameter, upperDiameter; };   // stacked cones/cylinders from the gauge line down
struct Holder { HolderId id; std::string name, vendor; std::vector<HolderSegment> segments; double gaugeLength; };

struct Material {
    MaterialId id; std::string name, group;    // "6061-T6", "Aluminium"
    double hardnessHb, specificCuttingForceNmm2, densityGcm3;
};

struct AxisDef { char letter; double minTravel, maxTravel; double rapidMmMin, maxFeedMmMin, accelMmS2; };
struct MachineDef {
    MachineId id; std::string name, vendor, model;
    enum class Kind { Mill3Axis, Mill4Axis, Mill5Axis, Lathe } kind;      // v1 implements Mill3Axis only
    std::vector<AxisDef> axes;
    double maxRpm, minRpm, spindlePowerKw, spindleTorqueNm;
    int toolCapacity; double toolChangeSec;
    std::string controlDialect;        // "haas-ngc","fanuc","mazak-eia","generic-iso"
    std::string defaultPost;           // file stem in posts/
    bool rigidTapping, throughSpindleCoolant, hasProbe, hasToolSetter;
    int maxBlockNumber; int maxProgramNameLength;
    std::map<std::string, std::string> postPropertyOverrides;   // machine-specific defaults for post properties
};

// Operation template (v1 shop knowledge): a saved operation with its tool and cutting data,
// keyed by what it is for. Applying one creates an ordinary, editable Operation.
struct OpTemplate {
    Uuid id; std::string name;
    std::string opType;                    // cam::Operation::type
    std::string featureKind;               // "hole.tapped","hole.plain","pocket.closed","face", ... or empty
    std::optional<MaterialId> material;    // empty = any
    std::optional<MachineId> machine;      // empty = any
    std::string toolQueryJson;             // e.g. {"type":"FlatEndMill","diameter":{"max":"0.6*feature.minWidth"}}
    std::string paramsJson;                // operation params; values may be expressions over feature.* and tool.*
};
// Shop rule (v2): ordered list of templates chosen by a condition on a recognised feature.
struct ShopRule { Uuid id; std::string name; int priority; std::string whenJson; std::vector<Uuid> templates; };

struct LibraryIssue { std::string file; std::string message; };

class ILibrary {
public:
    virtual ~ILibrary() = default;
    static Result<std::unique_ptr<ILibrary>> open(const std::string& rootDir);   // tolerant: bad files become issues, not failure
    virtual std::vector<LibraryIssue> issues() const = 0;
    virtual std::vector<Tool> tools() const = 0;
    virtual std::vector<Holder> holders() const = 0;
    virtual std::vector<Material> materials() const = 0;
    virtual std::vector<MachineDef> machines() const = 0;
    virtual std::vector<OpTemplate> templates() const = 0;
    virtual std::vector<std::string> postFiles() const = 0;
    // Writes are atomic (write temp file in the same folder, fsync, rename). If the file changed on disk
    // since it was read (mtime + content hash), returns ErrorCode::Locked with the newer content available.
    virtual Status save(const Tool&) = 0;
    virtual Status save(const Holder&) = 0;
    virtual Status save(const Material&) = 0;
    virtual Status save(const MachineDef&) = 0;
    virtual Status save(const OpTemplate&) = 0;
    virtual Status remove(std::string_view kind, const Uuid&) = 0;
    virtual void watch(std::function<void()> onChangedUiThread) = 0;    // polls every 5 s; network shares give no reliable notifications
};

// Several roots are layered: built-in (read-only, shipped) < shop (network) < user (local). Later wins by id.
class LibraryStack {
public:
    void setRoots(std::vector<std::string> dirsLowestFirst);
    const Tool* tool(const ToolId&) const;
    const Holder* holder(const HolderId&) const;
    const Material* material(const MaterialId&) const;
    const MachineDef* machine(const MachineId&) const;
    std::vector<Tool> queryTools(std::string_view toolQueryJson, const std::map<std::string, double>& vars) const;   // sorted by (diameter, id): deterministic
    std::vector<OpTemplate> templatesFor(std::string_view featureKind, const MaterialId&, const MachineId&) const;
};

} // namespace ray::lib
