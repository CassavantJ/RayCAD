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
// same JSON without whitespace). Keys a file has that this version does not know (fields of a newer RayCAD) are kept
// in the item (extraJson) and written back when it is saved again. Files whose names start with '.' are ignored
// (temporary files).
//
// Changes made in T-030 (each also in docs/interfaces/libraries.h):
//  - MachineDef::verified: shipped definitions say whether someone checked them on the real machine (PLAN 3.6).
//  - ILibrary::watch takes the IUiDispatcher that brings the callback to the UI thread (lib has no other way to
//    reach it), and ILibrary::refresh rescans on the calling thread.
//  - LibraryStack has storage (pimpl), issues() and refresh().
//  - toJson / *FromJson: the file format of one item, for embedding items in documents (T-032) and for tests.
// Changes made when the wave-1 CAM modules were joined (each also in docs/interfaces/libraries.h):
//  - Every member has a default (a default-constructed item holds no garbage).
//  - `comment` is a field of every item (and of a preset): the shipped files name the public source of their values
//    there, and the tool library UI shows it. `extraJson` carries the keys this version does not know, so an item
//    read from a library and embedded in a document keeps them: toJson writes them, and the document stores toJson,
//    so a library file and the same item embedded in a document are the same JSON.
//  - Tool::geometryHash and Holder::geometryHash: the shape alone, which is what an operation's input hash takes
//    from them (cam.h operationInputHash), so a rename or a cutting-data edit does not make toolpaths stale.
//  - ItemKind; ILibrary::remove takes it instead of a string; ILibrary::name.
//  - LibraryStack::queryTools takes string as well as number variables and returns its error instead of logging it.
//  - LibraryStack: read-only roots, roots() (name, open, writable), library(i) to save into, rootOf (where an item
//    comes from), merged lists of every kind, remerge, and one watch over every root with listeners (the tool
//    library UI, T-046).
// Changed by the canned-cycle settlement (cam.h): MachineDef::peckClearance, optional, the expansion fallback.
// Changed so start-up does not wait for network folders: LibraryStack::setRootsInBackground, RootInfo::loading.
// Changed for the feeds power model: Material::mc, optional, the chip thickness exponent of kc (kc1.1 alone reads about
// half the power at typical chip thicknesses).
// Changed for the tool library UI (T-046), so that neither a rescan nor a save of a network share runs on the UI
// thread: LibraryStack::refreshInBackground (refresh() on a worker, merged on the UI thread) and
// LibraryStack::sharedLibrary (a writable root's library that a worker may keep using across a later setRoots).
#pragma once
#include "core.h"
#include <chrono>
#include <map>
#include <optional>
#include <variant>

namespace ray::lib {

enum class ToolType { FlatEndMill, BallEndMill, BullEndMill, ChamferMill, FaceMill, SpotDrill, Drill, Tap,
                      Reamer, BoringBar, ThreadMill, EngraveCutter };

struct ToolPreset {              // per-material cutting data
    MaterialId material;
    std::string opClass;         // "rough","finish","slot","drill","tap" (schema lists the rest)
    double surfaceSpeedMMin = 0, chipLoadMm = 0, maxRadialFraction = 0, maxAxialFraction = 0, rampAngleDeg = 0;   // chip load per flute
    std::string comment;         // free text; written only when not empty
};
struct Tool {
    ToolId id;
    std::string name, vendor, productId;
    ToolType type = ToolType::FlatEndMill;
    int flutes = 0;
    double diameter = 0, shankDiameter = 0, fluteLength = 0, shoulderLength = 0, overallLength = 0;
    double cornerRadius = 0, tipAngleDeg = 0, tipDiameter = 0;   // bull / drill, chamfer, spot
    double threadPitch = 0; std::string threadSpec;              // taps and thread mills, e.g. "1/4-20 UNC", "M6x1"
    std::string bodyMaterial;                                    // "carbide","HSS"
    std::vector<ToolPreset> presets;
    int defaultToolNumber = 0;
    std::string comment;        // free text (the shipped library names its sources here); written only when not empty
    std::string extraJson;      // keys this version does not know, as a canonical JSON object ("" for none); kept
    Hash256 contentHash() const;    // BLAKE3 of toJson(*this): every field above, name, presets and extraJson included
    // BLAKE3 of the shape alone: type, flutes, the five diameters and lengths, corner radius, tip angle and diameter,
    // thread pitch. Not the id, names, vendor text, thread designation, body material, presets, tool number, comment
    // or unknown keys.
    Hash256 geometryHash() const;
};
struct HolderSegment { double height = 0, lowerDiameter = 0, upperDiameter = 0; };   // stacked cones/cylinders from the gauge line down
struct Holder {
    HolderId id; std::string name, vendor; std::vector<HolderSegment> segments; double gaugeLength = 0;
    std::string comment;        // as Tool::comment
    std::string extraJson;      // as Tool::extraJson
    Hash256 geometryHash() const;   // BLAKE3 of the gauge length and the segments, in order
};

struct Material {
    MaterialId id; std::string name, group;    // "6061-T6", "Aluminium"
    double hardnessHb = 0, specificCuttingForceNmm2 = 0, densityGcm3 = 0;
    // Chip thickness exponent: kc = kc1.1 h^-mc, h the mean chip thickness in mm (cam.h computeFeeds); 0 = not known
    // (kc1.1 is used flat). Optional (JSON "mc", written only when not 0); feeds round it to a multiple of 1/256.
    double mc = 0;
    std::string comment;        // as Tool::comment
    std::string extraJson;      // as Tool::extraJson
};

struct AxisDef { char letter = 'X'; double minTravel = 0, maxTravel = 0; double rapidMmMin = 0, maxFeedMmMin = 0, accelMmS2 = 0; };   // 0 = not known
struct MachineDef {
    MachineId id; std::string name, vendor, model;
    enum class Kind { Mill3Axis, Mill4Axis, Mill5Axis, Lathe } kind = Kind::Mill3Axis;   // v1 implements Mill3Axis only
    std::vector<AxisDef> axes;
    double maxRpm = 0, minRpm = 0, spindlePowerKw = 0, spindleTorqueNm = 0;   // 0 = not published / not known
    int toolCapacity = 0; double toolChangeSec = 0;
    std::string controlDialect;        // "haas-ngc","fanuc","mazak-smooth","generic-iso"
    std::string defaultPost;           // file stem in posts/
    bool rigidTapping = false, throughSpindleCoolant = false, hasProbe = false, hasToolSetter = false;
    int maxBlockNumber = 0; int maxProgramNameLength = 0;
    // The G83 re-entry clearance and G73 chip-break back-off the control is set to (a control parameter), mm; 0 = not
    // known. Optional (JSON control.peckClearance). Read only when a cycle is expanded into plain moves (cam.h,
    // peckClearanceNm); a native cycle leaves it to the control.
    double peckClearance = 0;
    // Machine-specific defaults for post properties. Each value is the property's JSON text ("true", "8", "\"x\"").
    std::map<std::string, std::string> postPropertyOverrides;
    bool verified = false;             // checked against the real machine; shipped definitions are false
    std::string comment;               // as Tool::comment
    std::string extraJson;             // as Tool::extraJson
};

// Operation template (v1 shop knowledge): a saved operation with its tool and cutting data,
// keyed by what it is for. Applying one creates an ordinary, editable Operation.
// Expressions in toolQueryJson and paramsJson name variables as $feature.x and $tool.x (canonical; the bare
// feature.x is accepted in numeric expressions too). A string-valued key is a variable only as a whole "$name".
struct OpTemplate {
    Uuid id; std::string name;
    std::string opType;                    // cam::Operation::type
    std::string featureKind;               // "hole.tapped","hole.plain","pocket.closed","face", ... or empty
    std::optional<MaterialId> material;    // empty = any
    std::optional<MachineId> machine;      // empty = any
    std::string toolQueryJson;             // e.g. {"type":"flatEndMill","diameter":{"max":"0.6 * $feature.minWidth"}}
    std::string paramsJson;                // operation params; values may be expressions over $feature.* and $tool.*
    std::string comment;                   // as Tool::comment
    std::string extraJson;                 // as Tool::extraJson
};
// Shop rule (v2): ordered list of templates chosen by a condition on a recognised feature.
struct ShopRule { Uuid id; std::string name; int priority = 0; std::string whenJson; std::vector<Uuid> templates; };

struct LibraryIssue { std::string file; std::string message; };

// The kinds of library item (ILibrary::remove, LibraryStack::rootOf).
enum class ItemKind { Tool, Holder, Material, Machine, Template };

// One item as the JSON of its file, without whitespace (canonical: sorted keys, shortest round-trip numbers), its
// unknown keys (extraJson) included, and back: the reader keeps the keys it does not know in extraJson (at any depth
// of nested objects; not inside arrays). The readers check the item's schema and return FormatError (FormatTooNew
// for a newer "schema" number) naming the first problem.
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
    virtual std::string name() const = 0;   // "name" of library.json as last read; "" when it has none or is unreadable
    // Writes are atomic (write temp file in the same folder, fsync, rename). If the file changed on disk
    // since it was read (its bytes are compared, not only its time stamp), or another process created or deleted
    // it, returns ErrorCode::Locked and the library holds the newer content from then on (so saving again
    // overwrites it). No file locks are taken. An item that would not pass its schema is InvalidArgument. The item's
    // extraJson is written, and so are keys of the file's last version that neither the item nor extraJson has.
    virtual Status save(const Tool&) = 0;
    virtual Status save(const Holder&) = 0;
    virtual Status save(const Material&) = 0;
    virtual Status save(const MachineDef&) = 0;
    virtual Status save(const OpTemplate&) = 0;
    // NotFound if the library has no such item.
    virtual Status remove(ItemKind, const Uuid&) = 0;
    // Rescans the folder now, on the calling thread; true if any item, issue or post file changed. A file whose
    // size and time stamp are unchanged is trusted only after two reads one rescan apart gave the same bytes, and
    // every 12th rescan reads every file again, so edits within one time stamp tick are found too.
    virtual bool refresh() = 0;
    // Calls refresh() every 5 s on a thread of its own (network shares give no reliable notifications) and posts
    // onChangedUiThread through `dispatcher` after a refresh that changed something. A second call replaces both;
    // the thread stops when the library is destroyed. `dispatcher` must outlive the library.
    virtual void watch(IUiDispatcher& dispatcher, std::function<void()> onChangedUiThread) = 0;
};

// The variables of a tool query: "feature.minWidth" -> 12.7, "feature.threadSpec" -> "1/4-20 UNC". Keys are spelled
// without the '$' that names them in a query.
using QueryValue = std::variant<double, std::string>;
using QueryVars = std::map<std::string, QueryValue, std::less<>>;

// Several roots are layered: built-in (read-only, shipped) < shop (network) < user (local). Later wins by id.
// Not thread-safe (UI thread). Item pointers returned stay valid until the next setRoots, refresh or remerge (or a
// change the watch merges, or a root read in the background arriving); ILibrary pointers until the next setRoots. A
// worker needs copies. setRootsInBackground keeps this: a worker reads a folder into a library nothing else sees, and
// only the UI thread puts it into the stack.
class LibraryStack {
public:
    LibraryStack();
    ~LibraryStack();
    LibraryStack(LibraryStack&&) noexcept;
    LibraryStack& operator=(LibraryStack&&) noexcept;
    // A root that cannot be opened becomes an issue. The first `readOnlyRoots` roots (the built-in library) are
    // read-only: roots() reports them not writable and library() does not hand them out. A watch carries over.
    void setRoots(std::vector<std::string> dirsLowestFirst, std::size_t readOnlyRoots = 0);
    // As setRoots, but only the first `openNow` roots are read here. The others are read on workers of `jobs`
    // (ILibrary::open, into a library no one else sees yet) and handed to the stack on the UI thread by the job's
    // continuation (posted through the job system's dispatcher), where each is merged in, watched when the stack is,
    // and the listeners are called. This call first waits up to `waitAtMost` for them (a local folder is read by then,
    // so the stack is usually complete when it returns; a slow or unreachable network folder does not hold it up
    // longer). Until a root arrives, roots() reports it `loading` and not open, and it adds no items; one that cannot
    // be opened becomes an issue, as with setRoots. A later setRoots or setRootsInBackground, or the stack's
    // destruction, cancels the reads still running (a read blocked inside the file system ends when that call
    // returns) and drops their results. `jobs` and its dispatcher must outlive the stack. Added so that start-up does
    // not wait for library.sharedFolders.
    void setRootsInBackground(std::vector<std::string> dirsLowestFirst, std::size_t readOnlyRoots, IJobSystem& jobs,
                              std::size_t openNow, std::chrono::milliseconds waitAtMost);
    std::vector<LibraryIssue> issues() const;                  // of every root, lowest root first
    bool refresh();                                            // rescans every root (and retries failed ones)
    // As refresh(), but the folders are read on a worker of `jobs`: every open root is rescanned there
    // (ILibrary::refresh) and a root that did not open (and is not loading) is opened again there. The job's
    // continuation (posted through the job system's dispatcher) merges on the UI thread, calls the listeners when
    // something changed and then `done` with whether anything did. Returns at once. A later setRoots or
    // setRootsInBackground, or the stack's destruction, cancels the job and drops its result (`done` is not called).
    // `jobs` and its dispatcher must outlive the stack.
    void refreshInBackground(IJobSystem& jobs, std::function<void(bool changed)> done = {});

    struct RootInfo {
        std::string dir;            // as given to setRoots
        std::string name;           // library.json "name", else the folder's own name
        bool open = false;          // false: it could not be opened (issues() says why; refresh() retries)
        bool writable = false;      // open and not read-only
        bool loading = false;       // being read in the background (setRootsInBackground); not open yet
    };
    std::vector<RootInfo> roots() const;                       // lowest first
    // The library of a writable root, to save into or remove from; null for a read-only root, one that is not open
    // or an index past the end. Call remerge() after changing it, so the merged lists show the change.
    ILibrary* library(std::size_t root);
    // The same library, shared: it stays alive (ILibrary is thread-safe) after a later setRoots replaced it or the
    // stack is gone, so a worker can save into it. Null where library() is. The caller calls remerge() on the UI
    // thread afterwards, as with library().
    std::shared_ptr<ILibrary> sharedLibrary(std::size_t root);
    // The merged lists again from what the libraries hold now, without reading any folder. Listeners are called.
    void remerge();
    // The root whose copy of the item the stack uses (the highest that has it); nullopt if no root has it.
    std::optional<std::size_t> rootOf(ItemKind, const Uuid&) const;

    // The merged items, sorted by id.
    std::vector<Tool> tools() const;
    std::vector<Holder> holders() const;
    std::vector<Material> materials() const;
    std::vector<MachineDef> machines() const;
    std::vector<OpTemplate> templates() const;
    const Tool* tool(const ToolId&) const;
    const Holder* holder(const HolderId&) const;
    const Material* material(const MaterialId&) const;
    const MachineDef* machine(const MachineId&) const;
    const OpTemplate* opTemplate(const Uuid&) const;

    // Query keys: "type" (a name or a list), the numeric fields (a value, an expression over `vars`, or
    // {"min","max"}), "threadSpec", "bodyMaterial", "material" and "opClass" (a preset for them). Numeric expressions
    // use + - * / and parentheses over numbers and number variables ($feature.minWidth; feature.minWidth works too);
    // the string keys take a literal or one string variable as the whole value ("$feature.threadSpec").
    // InvalidArgument naming the key and the problem for a malformed query, an unknown variable or one of the wrong
    // type. Sorted by (diameter, id): deterministic.
    Result<std::vector<Tool>> queryTools(std::string_view toolQueryJson, const QueryVars& vars) const;
    // Templates for the feature kind (empty: any) whose material and machine are empty or equal to the given ones,
    // the most specific first, then by name and id.
    std::vector<OpTemplate> templatesFor(std::string_view featureKind, const MaterialId&, const MachineId&) const;

    // Watching: every open root is rescanned every 5 s on a thread of its own (ILibrary::watch); a change is merged on
    // the UI thread, posted through `dispatcher`, and the listeners are called there. Roots set later are watched
    // too. `dispatcher` must outlive the stack. A second call replaces the dispatcher.
    void watch(IUiDispatcher& dispatcher);
    // Called on the UI thread after the merged lists may have changed: a change the watch found, setRoots, a refresh
    // that found a change, remerge. Returns 0 for an empty function.
    int subscribe(std::function<void()> onChanged);
    void unsubscribe(int);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ray::lib
