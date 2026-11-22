// RayCAD interface contract: shared libraries (tools, holders, materials, machines, templates, shop rules)
// Module: src/lib   Depends on: core
// Status: implemented in src/lib (task T-030). JSON schemas for every file type are in schemas/*.schema.json
// (tool, holder, material, machine, optemplate) and PLAN.md section 3.6.
//
// Storage: plain folders, one JSON file per item, so a shop can share a library on a network
// drive and two people rarely touch the same file. A document EMBEDS a copy of every library item it
// uses (tool, holder, machine, material), so a .raycad file is self-contained and reproducible.
//
//   <library root>/
//     tools/<uuid>.tool.json          holders/<uuid>.holder.json      materials/<uuid>.material.json
//     machines/<uuid>.machine.json    posts/<name>.post.js            templates/<uuid>.optemplate.json
//     rules/<uuid>.rule.json (v2)     library.json (name, format version)
//
// Files are written with sorted keys, two-space indentation and the canonical number form (toJson below is the
// same JSON without whitespace). Keys a file has that this version does not know (a "comment", fields of a newer
// RayCAD) are kept when the item is saved again. Files whose names start with '.' are ignored (temporary files).
//
// Changes made in T-030 (each also in docs/interfaces/libraries.h):
//  - MachineDef::verified: shipped definitions say whether someone checked them on the real machine (PLAN 3.6).
//  - ILibrary::watch takes the IUiDispatcher that brings the callback to the UI thread (lib has no other way to
//    reach it), and ILibrary::refresh rescans on the calling thread.
//  - LibraryStack has storage (pimpl), issues() and refresh().
//  - toJson / *FromJson: the file format of one item, for embedding items in documents (T-032) and for tests.
#pragma once
#include "core.h"
#include <map>
#include <optional>

namespace ray::lib {

enum class ToolType { FlatEndMill, BallEndMill, BullEndMill, ChamferMill, FaceMill, SpotDrill, Drill, Tap,
                      Reamer, BoringBar, ThreadMill, EngraveCutter };

struct ToolPreset {              // per-material cutting data
    MaterialId material;
    std::string opClass;         // "rough","finish","slot","drill","tap" (schema lists the rest)
    double surfaceSpeedMMin, chipLoadMm, maxRadialFraction, maxAxialFraction, rampAngleDeg;   // chip load per flute
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
    Hash256 contentHash() const;   // BLAKE3 of toJson(*this): every field above, presets and name included
};
struct HolderSegment { double height, lowerDiameter, upperDiameter; };   // stacked cones/cylinders from the gauge line down
struct Holder { HolderId id; std::string name, vendor; std::vector<HolderSegment> segments; double gaugeLength; };

struct Material {
    MaterialId id; std::string name, group;    // "6061-T6", "Aluminium"
    double hardnessHb, specificCuttingForceNmm2, densityGcm3;
};

struct AxisDef { char letter; double minTravel, maxTravel; double rapidMmMin, maxFeedMmMin, accelMmS2; };   // 0 = not known
struct MachineDef {
    MachineId id; std::string name, vendor, model;
    enum class Kind { Mill3Axis, Mill4Axis, Mill5Axis, Lathe } kind;      // v1 implements Mill3Axis only
    std::vector<AxisDef> axes;
    double maxRpm, minRpm, spindlePowerKw, spindleTorqueNm;               // 0 = not published / not known
    int toolCapacity; double toolChangeSec;
    std::string controlDialect;        // "haas-ngc","fanuc","mazak-eia","generic-iso"
    std::string defaultPost;           // file stem in posts/
    bool rigidTapping, throughSpindleCoolant, hasProbe, hasToolSetter;
    int maxBlockNumber; int maxProgramNameLength;
    // Machine-specific defaults for post properties. Each value is the property's JSON text ("true", "8", "\"x\"").
    std::map<std::string, std::string> postPropertyOverrides;
    bool verified = false;             // checked against the real machine; shipped definitions are false
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

// One item as the JSON of its file, without whitespace and without unknown keys (canonical: sorted keys, shortest
// round-trip numbers), and back. The readers check the item's schema and return FormatError (FormatTooNew for a
// newer "schema" number) naming the first problem.
std::string toJson(const Tool&);
std::string toJson(const Holder&);
std::string toJson(const Material&);
std::string toJson(const MachineDef&);
std::string toJson(const OpTemplate&);
Result<Tool> toolFromJson(std::string_view);
Result<Holder> holderFromJson(std::string_view);
Result<Material> materialFromJson(std::string_view);
Result<MachineDef> machineFromJson(std::string_view);
Result<OpTemplate> templateFromJson(std::string_view);

class ILibrary {
public:
    virtual ~ILibrary() = default;
    // tolerant: bad files become issues, not failure. NotFound if rootDir is not an existing folder.
    static Result<std::unique_ptr<ILibrary>> open(const std::string& rootDir);
    // Every accessor below is thread-safe and returns a copy, sorted by id (issues and postFiles by path).
    virtual std::vector<LibraryIssue> issues() const = 0;
    virtual std::vector<Tool> tools() const = 0;
    virtual std::vector<Holder> holders() const = 0;
    virtual std::vector<Material> materials() const = 0;
    virtual std::vector<MachineDef> machines() const = 0;
    virtual std::vector<OpTemplate> templates() const = 0;
    virtual std::vector<std::string> postFiles() const = 0;   // UTF-8 paths of posts/*.post.js
    // Writes are atomic (write temp file in the same folder, fsync, rename). If the file changed on disk
    // since it was read (its bytes are compared, not only its time stamp), or another process created or deleted
    // it, returns ErrorCode::Locked and the library holds the newer content from then on (so saving again
    // overwrites it). No file locks are taken. An item that would not pass its schema is InvalidArgument.
    virtual Status save(const Tool&) = 0;
    virtual Status save(const Holder&) = 0;
    virtual Status save(const Material&) = 0;
    virtual Status save(const MachineDef&) = 0;
    virtual Status save(const OpTemplate&) = 0;
    // kind: "tool","holder","material","machine","template". NotFound if the library has no such item.
    virtual Status remove(std::string_view kind, const Uuid&) = 0;
    // Rescans the folder now, on the calling thread; true if any item, issue or post file changed. A file whose
    // size and time stamp are unchanged is trusted only after two reads one rescan apart gave the same bytes, and
    // every 12th rescan reads every file again, so edits within one time stamp tick are found too.
    virtual bool refresh() = 0;
    // Calls refresh() every 5 s on a thread of its own (network shares give no reliable notifications) and posts
    // onChangedUiThread through `dispatcher` after a refresh that changed something. A second call replaces both;
    // the thread stops when the library is destroyed. `dispatcher` must outlive the library.
    virtual void watch(IUiDispatcher& dispatcher, std::function<void()> onChangedUiThread) = 0;
};

// Several roots are layered: built-in (read-only, shipped) < shop (network) < user (local). Later wins by id.
// Not thread-safe (UI thread). Pointers returned stay valid until the next setRoots or refresh.
class LibraryStack {
public:
    LibraryStack();
    ~LibraryStack();
    LibraryStack(LibraryStack&&) noexcept;
    LibraryStack& operator=(LibraryStack&&) noexcept;
    void setRoots(std::vector<std::string> dirsLowestFirst);   // a root that cannot be opened becomes an issue
    std::vector<LibraryIssue> issues() const;                  // of every root, lowest root first
    bool refresh();                                            // rescans every root (and retries failed ones)
    const Tool* tool(const ToolId&) const;
    const Holder* holder(const HolderId&) const;
    const Material* material(const MaterialId&) const;
    const MachineDef* machine(const MachineId&) const;
    // Query keys: "type" (a name or a list), the numeric fields (a value, an expression over `vars`, or
    // {"min","max"}), "threadSpec", "bodyMaterial", "material" and "opClass" (a preset for them). A malformed query
    // matches nothing and is logged.
    std::vector<Tool> queryTools(std::string_view toolQueryJson, const std::map<std::string, double>& vars) const;   // sorted by (diameter, id): deterministic
    // Templates for the feature kind (empty: any) whose material and machine are empty or equal to the given ones,
    // the most specific first, then by name and id.
    std::vector<OpTemplate> templatesFor(std::string_view featureKind, const MaterialId&, const MachineId&) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ray::lib
