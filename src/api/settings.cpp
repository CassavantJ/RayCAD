// User settings (ray_api/settings.h, task T-024): the key table, the store and its file, and the settings.* actions.
#include <ray_api/settings.h>

#include "app_impl.h"
#include "files.h"
#include "json.h"
#include "schema.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <mutex>
#include <system_error>
#include <utility>

namespace ray::api {
namespace {

using json::Value;

constexpr const char* kFormat = "raycad.settings";
constexpr const char* kFileName = "settings.json";

// ---------- The key table ----------
struct KeyDef {
    SettingInfo info;
    Value schema;
    Value def;            // the default; null: no value (a graphics override or glass: the tier preset decides)
    bool textual = false; // the value is a string (so a string is never coerced to a number or a boolean)
};

Value stringEnum(std::initializer_list<const char*> names)
{
    Value list = Value::array();
    for (const char* n : names) {
        list.push(Value::string(n));
    }
    return Value::object({{"type", Value::string("string")}, {"enum", std::move(list)}});
}

Value integerEnum(std::initializer_list<int> values)
{
    Value list = Value::array();
    for (const int v : values) {
        list.push(Value::integer(v));
    }
    return Value::object({{"type", Value::string("integer")}, {"enum", std::move(list)}});
}

Value integerRange(std::int64_t lo, std::int64_t hi)
{
    return Value::object(
        {{"type", Value::string("integer")}, {"minimum", Value::integer(lo)}, {"maximum", Value::integer(hi)}});
}

Value numberRange(double lo, double hi)
{
    return Value::object(
        {{"type", Value::string("number")}, {"minimum", Value::number(lo)}, {"maximum", Value::number(hi)}});
}

Value booleanSchema()
{
    return Value::object({{"type", Value::string("boolean")}});
}

Value stringPattern(const char* pattern, int maxLength)
{
    return Value::object({{"type", Value::string("string")},
                          {"pattern", Value::string(pattern)},
                          {"maxLength", Value::integer(maxLength)}});
}

std::vector<KeyDef> makeKeys()
{
    std::vector<KeyDef> k;
    const auto add = [&k](const char* key, const char* title, const char* description, Value schema, Value def) {
        KeyDef d;
        d.info.key = key;
        d.info.title = title;
        d.info.description = description;
        const Value* type = schema.find("type");
        d.textual = type != nullptr && type->isString() && type->asString() == "string";
        d.info.schemaJson = json::canonical(schema);
        d.info.defaultJson = json::canonical(def);
        d.schema = std::move(schema);
        d.def = std::move(def);
        k.push_back(std::move(d));
    };
    // ---- general ----
    add("units.newDocument", "Units of new documents",
        "The unit system a new document starts with (an open document keeps its own)", stringEnum({"inch", "metric"}),
        Value::string("inch"));
    add("units.decimalsInch", "Decimals (inch)", "Decimal places shown for lengths in inches", integerRange(0, 8),
        Value::integer(4));
    add("units.decimalsMetric", "Decimals (mm)", "Decimal places shown for lengths in millimetres", integerRange(0, 8),
        Value::integer(3));
    add("files.recentShown", "Recent files shown", "How many recent files the start screen lists", integerRange(1, 20),
        Value::integer(10));
    // ---- appearance ----
    add("appearance.theme", "Theme", "Follow the system's light or dark setting, or always use one",
        stringEnum({"system", "dark", "light"}), Value::string("system"));
    add("appearance.glass", "Glass panels",
        "Translucent glass panels over the 3D view, or flat opaque ones; unset follows the graphics tier",
        booleanSchema(), Value());
    // ---- graphics ----
    add("graphics.tier", "Graphics tier",
        "auto chooses from the detected hardware; low, medium and high force that tier",
        stringEnum({"auto", "low", "medium", "high"}), Value::string("auto"));
    add("graphics.msaaSamples", "Anti-aliasing samples",
        "Multisample anti-aliasing of the 3D view (0 is off); unset follows the tier", integerEnum({0, 2, 4, 8}),
        Value());
    add("graphics.smoothEdges", "Smooth edges", "Anti-aliased model edges; unset follows the tier", booleanSchema(),
        Value());
    add("graphics.transparency", "Transparency", "See-through bodies and stock; unset follows the tier",
        booleanSchema(), Value());
    add("graphics.sectionCaps", "Section caps", "Filled caps on section views; unset follows the tier", booleanSchema(),
        Value());
    add("graphics.ambientOcclusion", "Ambient occlusion", "Soft contact shadows; unset follows the tier",
        booleanSchema(), Value());
    add("graphics.shadows", "Shadows", "Cast shadows; unset follows the tier", booleanSchema(), Value());
    add("graphics.meshChordTolMm", "Display mesh chord tolerance",
        "Largest gap in mm between a displayed face and the true surface; unset follows the tier",
        numberRange(0.001, 1.0), Value());
    add("graphics.meshAngleTolDeg", "Display mesh angle tolerance",
        "Largest angle in degrees between neighbouring display triangles; unset follows the tier",
        numberRange(1.0, 45.0), Value());
    add("graphics.simCellMm", "Simulation cell size",
        "Upper bound of the material-removal simulation's cell size in mm; unset follows the tier",
        numberRange(0.01, 1.0), Value());
    add("graphics.maxBackplotSegments", "Backplot display limit",
        "Toolpath segments drawn before the display is thinned out; unset follows the tier",
        integerRange(100000, 100000000), Value());
    add("graphics.lastDetectedTier", "Detected tier", "The tier the last hardware detection chose (written by the app)",
        stringEnum({"low", "medium", "high"}), Value());
    add("graphics.lastDriverVersion", "Detected driver",
        "The graphics driver version the last hardware detection saw (written by the app)",
        stringPattern("^[ -~]*$", 128), Value());
    // ---- navigation ----
    add("navigation.preset", "Mouse navigation", "The mouse mapping of the 3D view (raycad, middle-orbit, right-orbit)",
        stringPattern("^[a-z0-9][a-z0-9-]*$", 64), Value::string("raycad"));
    // ---- window ----
    add("window.x", "Window left", "Left edge of the main window in logical pixels", integerRange(-100000, 100000),
        Value());
    add("window.y", "Window top", "Top edge of the main window in logical pixels", integerRange(-100000, 100000),
        Value());
    add("window.width", "Window width", "Width of the main window in logical pixels", integerRange(200, 100000),
        Value());
    add("window.height", "Window height", "Height of the main window in logical pixels", integerRange(200, 100000),
        Value());
    add("window.maximized", "Window maximized", "The main window opens maximized", booleanSchema(),
        Value::boolean(false));
    add("panels.model.open", "Model panel", "The model panel is shown", booleanSchema(), Value::boolean(true));
    add("panels.model.width", "Model panel width", "Width of the model panel; unset uses the theme's width",
        integerRange(100, 4000), Value());
    add("panels.properties.open", "Properties panel", "The properties panel is shown", booleanSchema(),
        Value::boolean(true));
    add("panels.properties.width", "Properties panel width",
        "Width of the properties panel; unset uses the theme's width", integerRange(100, 4000), Value());
    add("panels.properties.tab", "Right panel tab", "The tab the right panel shows: properties or measure",
        stringEnum({"properties", "measure"}), Value::string("properties"));
    return k;
}

const std::vector<KeyDef>& keyDefs()
{
    static const std::vector<KeyDef> defs = makeKeys();
    return defs;
}

const KeyDef* findDef(std::string_view key)
{
    for (const KeyDef& d : keyDefs()) {
        if (d.info.key == key) {
            return &d;
        }
    }
    return nullptr;
}

std::vector<std::string> splitKey(const std::string& key)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = key.find('.', start);
        parts.push_back(key.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
        if (dot == std::string::npos) {
            return parts;
        }
        start = dot + 1;
    }
}

const Value* findPath(const Value& doc, const std::string& key)
{
    const Value* at = &doc;
    for (const std::string& part : splitKey(key)) {
        at = at->find(part);
        if (at == nullptr) {
            return nullptr;
        }
    }
    return at;
}

void setPath(Value& doc, const std::string& key, Value value)
{
    const std::vector<std::string> parts = splitKey(key);
    Value* at = &doc;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        Value* next = at->find(parts[i]);
        if (next == nullptr || !next->isObject()) {
            next = &at->set(parts[i], Value::object());
        }
        at = next;
    }
    at->set(parts.back(), std::move(value));
}

// Removes the member and the objects it leaves empty.
void erasePath(Value& doc, const std::vector<std::string>& parts, std::size_t from = 0)
{
    if (from + 1 == parts.size()) {
        doc.erase(parts[from]);
        return;
    }
    Value* child = doc.find(parts[from]);
    if (child == nullptr || !child->isObject()) {
        return;
    }
    erasePath(*child, parts, from + 1);
    if (child->asObject().empty()) {
        doc.erase(parts[from]);
    }
}

// The value as the key's schema wants it, from what a person or the palette typed: "4" for 4, "true" for true,
// "" or "default" for null (unset).
Value coerce(const KeyDef& def, Value value)
{
    if (!value.isString()) {
        return value;
    }
    const std::string& text = value.asString();
    if (text.empty() || text == "default") {
        return Value();
    }
    if (def.textual) {
        return value;
    }
    auto parsed = json::parse(text);
    if (parsed && (parsed->isNumber() || parsed->isBool() || parsed->isNull())) {
        return std::move(*parsed);
    }
    return value;
}

Status validateValue(const KeyDef& def, const Value& value, const std::string& where)
{
    if (value.isNull()) {
        return {};
    }
    if (auto st = schema::validate(def.schema, value); !st) {
        Error e = st.error();
        constexpr std::string_view kWhole = "(the arguments)";
        if (e.message.compare(0, kWhole.size(), kWhole) == 0) {
            e.message = where + e.message.substr(kWhole.size());
        } else {
            e.message = where + e.message;
        }
        return tl::unexpected<Error>(std::move(e));
    }
    return {};
}

std::string displayName(const std::string& utf8Path)
{
    return detail::fileName(utf8Path);
}

} // namespace

// ---------- Names ----------
std::string_view unitSystemName(UnitSystem system)
{
    return system == UnitSystem::Inch ? "inch" : "metric";
}

std::string_view themeModeName(ThemeMode mode)
{
    switch (mode) {
    case ThemeMode::Dark:
        return "dark";
    case ThemeMode::Light:
        return "light";
    case ThemeMode::System:
        break;
    }
    return "system";
}

std::string_view tierChoiceName(TierChoice tier)
{
    switch (tier) {
    case TierChoice::Low:
        return "low";
    case TierChoice::Medium:
        return "medium";
    case TierChoice::High:
        return "high";
    case TierChoice::Auto:
        break;
    }
    return "auto";
}

std::optional<UnitSystem> unitSystemFromName(std::string_view name)
{
    if (name == "inch") {
        return UnitSystem::Inch;
    }
    if (name == "metric") {
        return UnitSystem::Metric;
    }
    return std::nullopt;
}

std::optional<ThemeMode> themeModeFromName(std::string_view name)
{
    for (const ThemeMode m : {ThemeMode::System, ThemeMode::Dark, ThemeMode::Light}) {
        if (themeModeName(m) == name) {
            return m;
        }
    }
    return std::nullopt;
}

std::optional<TierChoice> tierChoiceFromName(std::string_view name)
{
    for (const TierChoice t : {TierChoice::Auto, TierChoice::Low, TierChoice::Medium, TierChoice::High}) {
        if (tierChoiceName(t) == name) {
            return t;
        }
    }
    return std::nullopt;
}

// ---------- The store ----------
struct SettingsStore::Impl {
    std::string dir;  // "" = memory only
    std::string file; // dir/settings.json
    mutable std::mutex mutex;
    std::map<std::string, Value> values; // the keys the user set, validated
    Value extra = Value::object();       // members of the file this version does not know
    std::int64_t fileVersion = kVersion;
    std::optional<std::string> loadProblem, writeProblem;

    std::mutex listenersMutex;
    std::map<int, Listener> listeners;
    int nextListener = 1;

    const Value& effective(const KeyDef& def) const
    {
        auto it = values.find(def.info.key);
        return it != values.end() ? it->second : def.def;
    }
    UserSettings typed() const;
    void load();
    Value document() const;
    void write();
    void moveAside(const std::string& why);
};

void SettingsStore::Impl::moveAside(const std::string& why)
{
    const std::filesystem::path target = detail::toFsPath(file);
    std::filesystem::path aside =
        target.parent_path() / detail::toFsPath("settings.corrupt-" + std::to_string(detail::unixNowMs()) + ".json");
    std::error_code ec;
    std::filesystem::rename(target, aside, ec);
    const std::string kept = ec ? std::string() : displayName(detail::fromFsPath(aside));
    loadProblem = "The settings file could not be read (" + why + "). RayCAD started with the default settings" +
                  (kept.empty() ? std::string(".") : "; the old file was kept as " + kept + ".");
    log(LogLevel::Warn, "api", *loadProblem + " " + file);
}

void SettingsStore::Impl::load()
{
    if (file.empty()) {
        return;
    }
    std::error_code ec;
    const std::filesystem::path path = detail::toFsPath(file);
    if (!std::filesystem::exists(path, ec)) {
        return; // first start: the defaults
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        // Unreadable (permissions, locked): use the defaults this time but leave the file alone.
        loadProblem = "The settings file could not be opened; RayCAD started with the default settings.";
        log(LogLevel::Warn, "api", *loadProblem + " " + file);
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    auto parsed = json::parse(text);
    if (!parsed) {
        moveAside("it is not valid JSON: " + parsed.error().message);
        return;
    }
    if (!parsed->isObject()) {
        moveAside("it is not a JSON object");
        return;
    }
    Value doc = std::move(*parsed);
    if (const Value* format = doc.find("format");
        format != nullptr && (!format->isString() || format->asString() != kFormat)) {
        moveAside("it is not a RayCAD settings file");
        return;
    }
    if (const Value* version = doc.find("version"); version != nullptr && version->isInteger()) {
        fileVersion = static_cast<std::int64_t>(version->asNumber());
    }
    if (fileVersion > kVersion) {
        log(LogLevel::Info, "api",
            "The settings file was written by a newer RayCAD (version " + std::to_string(fileVersion) +
                "); settings this version does not know are kept unchanged");
    }
    // Migrations from older versions go here (version 1 is the first).
    doc.erase("format");
    doc.erase("version");
    for (const KeyDef& def : keyDefs()) {
        const Value* found = findPath(doc, def.info.key);
        if (found == nullptr) {
            continue;
        }
        if (!found->isNull()) {
            if (auto st = validateValue(def, *found, def.info.key); st) {
                values[def.info.key] = *found;
            } else {
                log(LogLevel::Warn, "api", "Ignored the setting " + st.error().message + " in " + file);
            }
        }
        erasePath(doc, splitKey(def.info.key));
    }
    extra = std::move(doc);
}

Value SettingsStore::Impl::document() const
{
    Value doc = extra;
    doc.set("format", Value::string(kFormat));
    doc.set("version", Value::integer(std::max<std::int64_t>(fileVersion, kVersion)));
    for (const KeyDef& def : keyDefs()) {
        if (auto it = values.find(def.info.key); it != values.end()) {
            setPath(doc, def.info.key, it->second);
        }
    }
    return doc;
}

void SettingsStore::Impl::write()
{
    if (file.empty()) {
        return;
    }
    const std::filesystem::path target = detail::toFsPath(file);
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    // A name of its own, so two processes saving at once never write into the same temporary file.
    std::filesystem::path temp = target;
    temp += detail::toFsPath(".tmp-" + Uuid::generate().toString());
    const std::string text = json::canonical(document()) + "\n";
    bool ok = false;
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << text;
        out.flush();
        ok = static_cast<bool>(out);
    }
    if (ok) {
        std::filesystem::rename(temp, target, ec);
        ok = !ec;
    }
    if (!ok) {
        std::filesystem::remove(temp, ec);
        writeProblem = "The settings could not be saved to " + file + "; the change applies until RayCAD closes.";
        log(LogLevel::Warn, "api", *writeProblem);
        return;
    }
    writeProblem.reset();
}

UserSettings SettingsStore::Impl::typed() const
{
    UserSettings s;
    const auto get = [this](const char* key) -> const Value& {
        return effective(*findDef(key));
    };
    const auto optBool = [&](const char* key) -> std::optional<bool> {
        const Value& v = get(key);
        return v.isBool() ? std::optional<bool>(v.asBool()) : std::nullopt;
    };
    const auto optInt = [&](const char* key) -> std::optional<int> {
        const Value& v = get(key);
        return v.isInteger() ? std::optional<int>(static_cast<int>(v.asNumber())) : std::nullopt;
    };
    const auto optNum = [&](const char* key) -> std::optional<double> {
        const Value& v = get(key);
        return v.isNumber() ? std::optional<double>(v.asNumber()) : std::nullopt;
    };
    const auto str = [&](const char* key) -> std::string {
        const Value& v = get(key);
        return v.isString() ? v.asString() : std::string();
    };
    s.newDocumentUnits = unitSystemFromName(str("units.newDocument")).value_or(UnitSystem::Inch);
    s.decimalsInch = optInt("units.decimalsInch").value_or(4);
    s.decimalsMetric = optInt("units.decimalsMetric").value_or(3);
    s.recentFilesShown = optInt("files.recentShown").value_or(10);
    s.theme = themeModeFromName(str("appearance.theme")).value_or(ThemeMode::System);
    s.glass = optBool("appearance.glass");
    s.tier = tierChoiceFromName(str("graphics.tier")).value_or(TierChoice::Auto);
    s.overrides.msaaSamples = optInt("graphics.msaaSamples");
    s.overrides.smoothEdges = optBool("graphics.smoothEdges");
    s.overrides.transparency = optBool("graphics.transparency");
    s.overrides.sectionCaps = optBool("graphics.sectionCaps");
    s.overrides.ambientOcclusion = optBool("graphics.ambientOcclusion");
    s.overrides.shadows = optBool("graphics.shadows");
    s.overrides.meshChordTolMm = optNum("graphics.meshChordTolMm");
    s.overrides.meshAngleTolDeg = optNum("graphics.meshAngleTolDeg");
    s.overrides.simCellMm = optNum("graphics.simCellMm");
    s.overrides.maxBackplotSegments = optInt("graphics.maxBackplotSegments");
    if (auto detected = tierChoiceFromName(str("graphics.lastDetectedTier"))) {
        s.lastDetectedTier = *detected;
    }
    s.lastDriverVersion = str("graphics.lastDriverVersion");
    s.navigationPreset = str("navigation.preset");
    const auto x = optInt("window.x");
    const auto y = optInt("window.y");
    const auto w = optInt("window.width");
    const auto h = optInt("window.height");
    if (x && y && w && h) {
        s.window = WindowGeometry{*x, *y, *w, *h, optBool("window.maximized").value_or(false)};
    }
    for (const char* panel : {"model", "properties"}) {
        const std::string base = std::string("panels.") + panel;
        PanelState p;
        p.open = optBool((base + ".open").c_str()).value_or(true);
        p.width = optInt((base + ".width").c_str());
        s.panels[panel] = p;
    }
    return s;
}

SettingsStore::SettingsStore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

SettingsStore::~SettingsStore() = default;

std::shared_ptr<SettingsStore> SettingsStore::open(const std::optional<std::string>& userDataDir)
{
    static std::mutex registryMutex;
    static std::map<std::string, std::weak_ptr<SettingsStore>> registry;
    const std::string dir = detail::resolveUserDataDir(userDataDir);
    const auto make = [&dir]() {
        auto impl = std::make_unique<Impl>();
        impl->dir = dir;
        if (!dir.empty()) {
            impl->file = detail::fromFsPath(detail::toFsPath(dir) / kFileName);
        }
        impl->load();
        return std::shared_ptr<SettingsStore>(new SettingsStore(std::move(impl)));
    };
    if (dir.empty()) {
        return make();
    }
    const std::string key = detail::pathKey(dir);
    std::lock_guard<std::mutex> lock(registryMutex);
    if (auto it = registry.find(key); it != registry.end()) {
        if (auto existing = it->second.lock()) {
            return existing;
        }
    }
    auto store = make();
    registry[key] = store;
    return store;
}

const std::vector<SettingInfo>& SettingsStore::keys()
{
    static const std::vector<SettingInfo> infos = [] {
        std::vector<SettingInfo> out;
        for (const KeyDef& d : keyDefs()) {
            out.push_back(d.info);
        }
        return out;
    }();
    return infos;
}

const SettingInfo* SettingsStore::find(std::string_view key)
{
    const KeyDef* d = findDef(key);
    return d ? &d->info : nullptr;
}

std::string SettingsStore::filePath() const
{
    return impl_->file;
}

std::string SettingsStore::directory() const
{
    return impl_->dir;
}

std::optional<std::string> SettingsStore::loadProblem() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->loadProblem;
}

std::optional<std::string> SettingsStore::writeProblem() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->writeProblem;
}

UserSettings SettingsStore::get() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->typed();
}

bool SettingsStore::isSet(std::string_view key) const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->values.count(std::string(key)) > 0;
}

std::vector<std::string> SettingsStore::setKeys() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<std::string> out;
    for (const KeyDef& d : keyDefs()) {
        if (impl_->values.count(d.info.key) > 0) {
            out.push_back(d.info.key);
        }
    }
    return out;
}

Result<std::string> SettingsStore::valueJson(std::string_view key) const
{
    const KeyDef* def = findDef(key);
    if (def == nullptr) {
        return detail::fail(ErrorCode::NotFound, "There is no setting named '" + std::string(key) + "'");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return json::canonical(impl_->effective(*def));
}

std::string SettingsStore::valuesJson() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Value out = Value::object();
    for (const KeyDef& d : keyDefs()) {
        out.set(d.info.key, impl_->effective(d));
    }
    return json::canonical(out);
}

std::string SettingsStore::documentJson() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return json::canonical(impl_->document());
}

Status SettingsStore::setMany(std::string_view objectJson)
{
    auto parsed = json::parse(objectJson);
    if (!parsed || !parsed->isObject()) {
        return detail::fail(ErrorCode::InvalidArgument, "The settings must be a JSON object of key and value");
    }
    // Validate everything first: all or nothing.
    std::vector<std::pair<const KeyDef*, Value>> changes;
    for (const auto& [key, raw] : parsed->asObject()) {
        const KeyDef* def = findDef(key);
        if (def == nullptr) {
            return detail::fail(ErrorCode::NotFound, "There is no setting named '" + key + "'");
        }
        Value value = coerce(*def, raw);
        if (auto st = validateValue(*def, value, schema::pointerAppend("", key)); !st) {
            return st;
        }
        changes.emplace_back(def, std::move(value));
    }
    std::vector<std::string> changed;
    UserSettings after;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (auto& [def, value] : changes) {
            auto it = impl_->values.find(def->info.key);
            const bool wasSet = it != impl_->values.end();
            if (value.isNull()) {
                if (wasSet) {
                    impl_->values.erase(it);
                    changed.push_back(def->info.key);
                }
            } else if (!wasSet || !(it->second == value)) {
                impl_->values[def->info.key] = std::move(value);
                changed.push_back(def->info.key);
            }
        }
        if (changed.empty()) {
            return {};
        }
        impl_->write();
        after = impl_->typed();
    }
    // keys() order, no duplicates (a key named twice in one object counts once).
    std::vector<std::string> ordered;
    for (const KeyDef& d : keyDefs()) {
        if (std::find(changed.begin(), changed.end(), d.info.key) != changed.end()) {
            ordered.push_back(d.info.key);
        }
    }
    std::vector<Listener> listeners;
    {
        std::lock_guard<std::mutex> lock(impl_->listenersMutex);
        for (auto& [id, l] : impl_->listeners) {
            listeners.push_back(l);
        }
    }
    for (auto& l : listeners) {
        try {
            l(after, ordered);
        } catch (...) {
            log(LogLevel::Warn, "api", "A settings listener threw; ignored");
        }
    }
    return {};
}

Status SettingsStore::set(std::string_view key, std::string_view valueJson)
{
    const KeyDef* def = findDef(key);
    if (def == nullptr) {
        return detail::fail(ErrorCode::NotFound, "There is no setting named '" + std::string(key) + "'");
    }
    auto value = json::parse(valueJson);
    if (!value) {
        return detail::fail(ErrorCode::InvalidArgument, "/value: not valid JSON");
    }
    Value coerced = coerce(*def, std::move(*value));
    if (auto st = validateValue(*def, coerced, "/value"); !st) {
        return st;
    }
    Value one = Value::object();
    one.set(def->info.key, std::move(coerced));
    return setMany(json::write(one));
}

Status SettingsStore::reset(std::string_view keyOrPrefix)
{
    std::string prefix(keyOrPrefix);
    if (!prefix.empty() && prefix.back() == '.') {
        prefix.pop_back();
    }
    Value nulls = Value::object();
    bool matched = false;
    for (const KeyDef& d : keyDefs()) {
        const std::string& k = d.info.key;
        const bool match =
            prefix.empty() || k == prefix ||
            (k.size() > prefix.size() && k.compare(0, prefix.size(), prefix) == 0 && k[prefix.size()] == '.');
        if (match) {
            matched = true;
            nulls.set(k, Value());
        }
    }
    if (!matched) {
        return detail::fail(ErrorCode::NotFound, "There is no setting or group named '" + prefix + "'");
    }
    return setMany(json::write(nulls));
}

int SettingsStore::subscribe(Listener listener)
{
    if (!listener) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(impl_->listenersMutex);
    const int id = impl_->nextListener++;
    impl_->listeners[id] = std::move(listener);
    return id;
}

void SettingsStore::unsubscribe(int id)
{
    std::lock_guard<std::mutex> lock(impl_->listenersMutex);
    impl_->listeners.erase(id);
}

std::shared_ptr<SettingsStore> settingsOf(IApp& app)
{
    auto* impl = dynamic_cast<detail::AppImpl*>(&app);
    return impl != nullptr ? impl->settings() : nullptr;
}

// ---------- Actions ----------
namespace detail {
namespace {

Value keyEnumSchema(const char* description)
{
    Value list = Value::array();
    for (const KeyDef& d : keyDefs()) {
        list.push(Value::string(d.info.key));
    }
    return Value::object(
        {{"type", Value::string("string")}, {"enum", std::move(list)}, {"description", Value::string(description)}});
}

std::string objectSchema(Value properties, Value extra = Value::object())
{
    Value schema = Value::object({{"$schema", Value::string("https://json-schema.org/draft/2020-12/schema")},
                                  {"type", Value::string("object")},
                                  {"additionalProperties", Value::boolean(false)},
                                  {"properties", std::move(properties)}});
    for (auto& [k, v] : extra.asObject()) {
        schema.set(k, v);
    }
    return json::canonical(schema);
}

Result<Value> parseObject(std::string_view text)
{
    auto parsed = json::parse(text);
    if (!parsed || !parsed->isObject()) {
        return fail(ErrorCode::InvalidArgument, "The arguments must be a JSON object");
    }
    return std::move(*parsed);
}

Value parsedOrNull(const std::string& text)
{
    auto v = json::parse(text);
    return v ? std::move(*v) : Value();
}

Value parsedOrNull(const Result<std::string>& text)
{
    return text ? parsedOrNull(*text) : Value();
}

ActionResult resultOf(const Value& v, const SettingsStore& store)
{
    ActionResult r{json::canonical(v), {}};
    if (auto problem = store.writeProblem()) {
        r.warnings.push_back(*problem);
    }
    return r;
}

Value stringList(const std::vector<std::string>& items)
{
    Value list = Value::array();
    for (const std::string& s : items) {
        list.push(Value::string(s));
    }
    return list;
}

} // namespace

Status registerSettingsActions(IActionRegistry& actions, std::shared_ptr<SettingsStore> store)
{
    if (!store) {
        return fail(ErrorCode::InvalidArgument, "No settings store");
    }
    Status st = actions.add(
        ActionInfo{"settings.get", "Show Settings", "Settings",
                   "Lists every user setting with its value, or one setting with its default",
                   objectSchema(Value::object({{"key", keyEnumSchema("One setting; every setting when omitted")}})), "",
                   false, false, false},
        [store](std::string_view argsJson, Caller, JobContext&) -> Result<ActionResult> {
            auto args = parseObject(argsJson);
            if (!args) {
                return tl::unexpected<Error>(args.error());
            }
            const std::string key = args->stringOr("key", "");
            if (key.empty()) {
                Value out = Value::object();
                out.set("file", Value::string(store->filePath()));
                out.set("values", parsedOrNull(store->valuesJson()));
                out.set("set", stringList(store->setKeys()));
                return resultOf(out, *store);
            }
            const SettingInfo* info = SettingsStore::find(key);
            auto value = store->valueJson(key);
            if (info == nullptr || !value) {
                return fail(ErrorCode::NotFound, "There is no setting named '" + key + "'");
            }
            Value out = Value::object();
            out.set("key", Value::string(key));
            out.set("title", Value::string(info->title));
            out.set("value", parsedOrNull(value));
            out.set("default", parsedOrNull(info->defaultJson));
            out.set("set", Value::boolean(store->isSet(key)));
            return resultOf(out, *store);
        });
    if (!st) {
        return st;
    }

    Value value =
        Value::object({{"type", Value::array({Value::string("string"), Value::string("number"),
                                              Value::string("boolean"), Value::string("null")})},
                       {"description", Value::string("The new value (\"default\" or null: back to the default)")}});
    Value many = Value::object(
        {{"type", Value::string("object")},
         {"description", Value::string("Several settings at once, {\"key\": value, ...}; all or none are applied")}});
    Value anyOf = Value::array({Value::object({{"required", Value::array({Value::string("key")})}}),
                                Value::object({{"required", Value::array({Value::string("values")})}})});
    st = actions.add(ActionInfo{"settings.set", "Change Setting", "Settings",
                                "Changes a user setting; it applies at once and is saved per user",
                                objectSchema(Value::object({{"key", keyEnumSchema("The setting to change")},
                                                            {"value", std::move(value)},
                                                            {"values", std::move(many)}}),
                                             Value::object({{"anyOf", std::move(anyOf)}})),
                                "", false, false, false},
                     [store](std::string_view argsJson, Caller, JobContext&) -> Result<ActionResult> {
                         auto args = parseObject(argsJson);
                         if (!args) {
                             return tl::unexpected<Error>(args.error());
                         }
                         const Value* key = args->find("key");
                         const Value* values = args->find("values");
                         if (key != nullptr && values != nullptr) {
                             return fail(ErrorCode::InvalidArgument, "Pass either key and value, or values");
                         }
                         std::vector<std::string> names;
                         if (key != nullptr) {
                             const Value* v = args->find("value");
                             if (v == nullptr) {
                                 return fail(ErrorCode::InvalidArgument, "/value: required with key");
                             }
                             if (auto done = store->set(key->asString(), json::write(*v)); !done) {
                                 return tl::unexpected<Error>(done.error());
                             }
                             names.push_back(key->asString());
                         } else {
                             if (auto done = store->setMany(json::write(*values)); !done) {
                                 Error e = done.error();
                                 if (e.code == ErrorCode::InvalidArgument && !e.message.empty() &&
                                     e.message.front() == '/') {
                                     e.message = "/values" + e.message;
                                 }
                                 return tl::unexpected<Error>(std::move(e));
                             }
                             for (const auto& [k, v] : values->asObject()) {
                                 names.push_back(k);
                             }
                         }
                         Value now = Value::object();
                         for (const std::string& n : names) {
                             now.set(n, parsedOrNull(store->valueJson(n)));
                         }
                         Value out = Value::object();
                         out.set("values", std::move(now));
                         out.set("file", Value::string(store->filePath()));
                         out.set("saved", Value::boolean(!store->writeProblem().has_value()));
                         return resultOf(out, *store);
                     });
    if (!st) {
        return st;
    }

    Value resetKey = Value::object(
        {{"type", Value::string("string")},
         {"minLength", Value::integer(1)},
         {"description",
          Value::string(
              "A setting (\"graphics.msaaSamples\"), a group (\"graphics\"), or \"all\" for every setting")}});
    return actions.add(ActionInfo{"settings.reset", "Reset Settings", "Settings",
                                  "Puts a setting, a group of settings or every setting back to its default",
                                  objectSchema(Value::object({{"key", std::move(resetKey)}}),
                                               Value::object({{"required", Value::array({Value::string("key")})}})),
                                  "", false, false, false},
                       [store](std::string_view argsJson, Caller, JobContext&) -> Result<ActionResult> {
                           auto args = parseObject(argsJson);
                           if (!args) {
                               return tl::unexpected<Error>(args.error());
                           }
                           std::string key = args->stringOr("key", "");
                           if (key == "all") {
                               key.clear();
                           }
                           const std::vector<std::string> before = store->setKeys();
                           if (auto done = store->reset(key); !done) {
                               return tl::unexpected<Error>(done.error());
                           }
                           const std::vector<std::string> after = store->setKeys();
                           std::vector<std::string> cleared;
                           for (const std::string& k : before) {
                               if (std::find(after.begin(), after.end(), k) == after.end()) {
                                   cleared.push_back(k);
                               }
                           }
                           Value out = Value::object();
                           out.set("reset", stringList(cleared));
                           return resultOf(out, *store);
                       });
}

} // namespace detail
} // namespace ray::api
