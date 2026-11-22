// User settings (task T-024): a typed, versioned per-user store, persisted as JSON in <user data dir>/settings.json,
// with change notification and the actions settings.get, settings.set and settings.reset (registered by every app
// made by makeApp / makeHeadlessApp, so the command palette and raycad-cli can use them). No Qt here.
// Not one of the contract headers in include/ray/; a later interface revision may move it there (and add an
// AppEvent::Kind for settings changes, which api.h does not have).
//
// The settings are addressed by dotted keys ("units.newDocument", "graphics.msaaSamples"); keys() lists them with
// their JSON Schema and default. The file holds only what the user changed, nested by the key's parts:
//   {"format":"raycad.settings","version":1,"units":{"newDocument":"metric"},"graphics":{"tier":"high"}}
// A key that is not in the file has its default; for the per-option graphics overrides and the glass setting the
// default is "whatever the tier preset says" (no value). Members the store does not know (written by a newer
// RayCAD) are kept when it writes the file.
//
// Robustness: a missing file is the defaults. A file that is not a JSON object is moved aside (to
// settings.corrupt-<ms since 1970>.json next to it) and the defaults are used; loadProblem() says so. A value that
// fails its key's schema is ignored (logged) and the key keeps its default. Writes go to a temporary file in the same
// folder that then replaces settings.json, so a crash never leaves half a file. Nothing here needs admin rights: the
// only folder written is the per-user data folder.
#pragma once

#include <ray/api.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ray::api {

enum class ThemeMode : std::uint8_t { System, Dark, Light };
// "auto": the tier hardware detection chooses (render::chooseTier); the others force that tier.
enum class TierChoice : std::uint8_t { Auto, Low, Medium, High };

// Overrides of single fields of the tier preset (render::TierPreset); nullopt keeps the preset's value. Display and
// simulation resolution only: none of them reaches toolpaths or G-code (PLAN.md 8).
struct PresetOverrides {
    std::optional<int> msaaSamples; // 0, 2, 4 or 8 (the renderer clamps to what the device supports)
    std::optional<bool> smoothEdges, transparency, sectionCaps, ambientOcclusion, shadows;
    std::optional<double> meshChordTolMm, meshAngleTolDeg;
    std::optional<double> simCellMm;
    std::optional<int> maxBackplotSegments;
};

struct WindowGeometry {
    int x = 0, y = 0, width = 0, height = 0; // logical pixels, the window's frame-less geometry
    bool maximized = false;
};

struct PanelState {
    bool open = true;
    std::optional<int> width; // logical pixels; nullopt: the theme's side-panel width
};

struct UserSettings {
    // ---- general ----
    UnitSystem newDocumentUnits = UnitSystem::Inch; // DocSettings::units of the next new document (PLAN.md 0.3)
    int decimalsInch = 4, decimalsMetric = 3;       // display decimals per unit system
    int recentFilesShown = 10;                      // 1..20; the app keeps at most 20 (IApp::recentFiles)
    // ---- appearance ----
    ThemeMode theme = ThemeMode::System;
    std::optional<bool> glass; // nullopt: the tier preset's liquidGlass
    // ---- graphics ----
    TierChoice tier = TierChoice::Auto;
    PresetOverrides overrides;
    // The last hardware detection (written by the desktop app; lets the next start use the right tier before
    // detection has run again). nullopt until the first detection.
    std::optional<TierChoice> lastDetectedTier;
    std::string lastDriverVersion;
    // ---- navigation ----
    std::string navigationPreset = "raycad"; // a ui navigation preset id
    // ---- window ----
    std::optional<WindowGeometry> window;     // nullopt until the window was closed once
    std::map<std::string, PanelState> panels; // "model", "properties": every known panel is listed
    // ---- library (the app's default library stack, api.h AppOptions::libraryRoots) ----
    std::string userLibraryFolder;                 // "library.userFolder"; "" = <user data dir>/library
    std::vector<std::string> sharedLibraryFolders; // "library.sharedFolders", lowest first

    int decimalsFor(UnitSystem system) const { return system == UnitSystem::Inch ? decimalsInch : decimalsMetric; }
    UnitFormat unitFormat(UnitSystem system) const { return UnitFormat{system, decimalsFor(system)}; }
};

struct SettingInfo {
    std::string key;         // "units.newDocument"
    std::string title;       // "Units of new documents"
    std::string description; // one sentence
    std::string schemaJson;  // JSON Schema of the value
    std::string defaultJson; // "null" when the default is "no value" (an override that is not set)
};

class SettingsStore {
public:
    static constexpr int kVersion = 1;
    // (settings, the keys whose value or explicitness changed, in keys() order)
    using Listener = std::function<void(const UserSettings&, const std::vector<std::string>& changedKeys)>;

    // The store of a user data folder. `userDataDir` as AppOptions::userDataDir: nullopt resolves the default
    // folder ($RAYCAD_USER_DATA_DIR, else the platform's per-user folder), "" persists nothing (memory only).
    // Every caller that opens the same folder in a process gets the same store (the desktop UI opens it before the
    // app exists; the app then shares it). Reads the file (it is small); never fails.
    static std::shared_ptr<SettingsStore> open(const std::optional<std::string>& userDataDir);
    // Every known key, in a fixed order.
    static const std::vector<SettingInfo>& keys();
    static const SettingInfo* find(std::string_view key);

    ~SettingsStore();
    SettingsStore(const SettingsStore&) = delete;
    SettingsStore& operator=(const SettingsStore&) = delete;

    // The file (UTF-8), or "" for a memory-only store; and the folder it is in.
    std::string filePath() const;
    std::string directory() const;
    // Set when the file could not be used at start (unreadable or not a settings object): a message for the user.
    std::optional<std::string> loadProblem() const;
    // Set when the last write failed (the change is kept in memory); cleared by the next successful write.
    std::optional<std::string> writeProblem() const;

    // ---- reading (any thread) ----
    UserSettings get() const;
    bool isSet(std::string_view key) const;   // the user set it (it is in the file)
    std::vector<std::string> setKeys() const; // in keys() order
    // The value in effect as canonical JSON (its default when not set; "null" for an unset override). NotFound
    // for an unknown key.
    Result<std::string> valueJson(std::string_view key) const;
    // {"<key>": value in effect, ...} for every key, canonical JSON.
    std::string valuesJson() const;
    // What the file holds (or would hold, for a memory-only store), canonical JSON.
    std::string documentJson() const;

    // ---- changing (call from one thread at a time; listeners run on the calling thread, after the change) ----
    // Sets one key. `valueJson` must satisfy the key's schema; JSON null removes the key (back to its default).
    // A string is accepted for a number, integer or boolean key when it reads as one ("4", "true"); "default" and
    // "" mean null. InvalidArgument "/value: ..." for a bad value, NotFound for an unknown key.
    Status set(std::string_view key, std::string_view valueJson);
    // Several keys at once ({"key": value, ...}); all or nothing. Errors name the bad member ("/window.width: ...").
    Status setMany(std::string_view objectJson);
    // Removes a key, every key under a prefix ("graphics" or "graphics."), or with "" every key. NotFound if no key
    // matches.
    Status reset(std::string_view keyOrPrefix);

    int subscribe(Listener listener); // 0 if empty
    void unsubscribe(int id);

private:
    struct Impl;
    explicit SettingsStore(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// The settings store of an app made by makeApp / makeHeadlessApp (the store of its IApp::userDataDir); null for
// any other IApp.
std::shared_ptr<SettingsStore> settingsOf(IApp& app);

// Text forms used in the file and by the actions: "inch"/"metric", "system"/"dark"/"light", "auto"/"low"/...
std::string_view unitSystemName(UnitSystem system);
std::string_view themeModeName(ThemeMode mode);
std::string_view tierChoiceName(TierChoice tier);
std::optional<UnitSystem> unitSystemFromName(std::string_view name);
std::optional<ThemeMode> themeModeFromName(std::string_view name);
std::optional<TierChoice> tierChoiceFromName(std::string_view name);

} // namespace ray::api
