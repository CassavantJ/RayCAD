// User settings (task T-024, ray_api/settings.h): defaults, persistence per user across app restarts, the file's
// shape, tolerance of corrupt, invalid and newer files, the settings.* actions and their validation, change
// notification, and the default unit system of new documents.
#include "api_test_util.h"

#include <ray_api/settings.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using ray::ErrorCode;
using ray::UnitSystem;
using ray::api::Caller;
using ray::api::SettingsStore;
using ray::api::TierChoice;
using ray::test::kWait;
using ray::test::makeTestApp;
using ray::test::TempDir;

namespace {

std::string readText(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<std::filesystem::path> filesIn(const std::filesystem::path& dir)
{
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        out.push_back(entry.path());
    }
    return out;
}

ray::Result<ray::api::ActionResult> run(ray::api::HeadlessApp& app, const char* id, const std::string& args)
{
    return app.wait(app.actions().invoke(id, args, Caller::User), kWait);
}

// Sets an environment variable for the life of the object and restores the old value.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::string& value) : name_(name)
    {
        if (const char* old = std::getenv(name)) {
            old_ = std::string(old);
        }
        put(value);
    }
    ~ScopedEnv() { put(old_.value_or(std::string())); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    void put(const std::string& value)
    {
#ifdef _WIN32
        _putenv_s(name_, value.c_str());
#else
        if (value.empty()) {
            unsetenv(name_);
        } else {
            setenv(name_, value.c_str(), 1);
        }
#endif
    }
    const char* name_;
    std::optional<std::string> old_;
};

} // namespace

TEST_CASE("settings start at their defaults and need no file", "[api][settings]")
{
    TempDir dir;
    auto store = SettingsStore::open(dir.file("user"));
    REQUIRE(store);
    const ray::api::UserSettings s = store->get();
    CHECK(s.newDocumentUnits == UnitSystem::Inch); // PLAN.md 0.3
    CHECK(s.decimalsInch == 4);
    CHECK(s.decimalsMetric == 3);
    CHECK(s.theme == ray::api::ThemeMode::System);
    CHECK_FALSE(s.glass.has_value()); // the tier preset decides
    CHECK(s.tier == TierChoice::Auto);
    CHECK_FALSE(s.overrides.msaaSamples.has_value());
    CHECK(s.navigationPreset == "raycad");
    CHECK_FALSE(s.window.has_value());
    CHECK(s.panels.at("model").open);
    CHECK(s.panels.at("properties").open);
    CHECK(s.recentFilesShown == 10);
    CHECK_FALSE(store->loadProblem().has_value());
    CHECK(store->setKeys().empty());
    // Reading writes nothing.
    CHECK_FALSE(std::filesystem::exists(dir.path() / "user" / "settings.json"));
    // Every key has a schema and a default, and valuesJson lists each one.
    for (const ray::api::SettingInfo& info : SettingsStore::keys()) {
        INFO(info.key);
        CHECK_FALSE(info.schemaJson.empty());
        CHECK_FALSE(info.defaultJson.empty());
        CHECK_FALSE(info.title.empty());
        CHECK_THAT(store->valuesJson(), ContainsSubstring("\"" + info.key + "\""));
    }
    // A memory-only store ("" = nothing persisted) works the same.
    auto memory = SettingsStore::open(std::string());
    REQUIRE(memory->set("units.decimalsInch", "2"));
    CHECK(memory->get().decimalsInch == 2);
    CHECK(memory->filePath().empty());
}

TEST_CASE("settings persist per user across app restarts", "[api][settings]")
{
    TempDir dir;
    const std::string user = dir.file("profile");
    // The app finds the folder through RAYCAD_USER_DATA_DIR, as the desktop app does.
    ScopedEnv env("RAYCAD_USER_DATA_DIR", user);
    {
        auto app = ray::api::makeHeadlessApp(ray::api::AppOptions{});
        REQUIRE(app);
        CHECK(app->userDataDir() == user);
        REQUIRE(run(*app, "settings.set", R"({"key":"units.newDocument","value":"metric"})"));
        REQUIRE(run(*app, "settings.set", R"({"key":"graphics.tier","value":"high"})"));
        REQUIRE(run(*app, "settings.set",
                    R"({"values":{"appearance.theme":"light","appearance.glass":true,"graphics.msaaSamples":2,)"
                    R"("navigation.preset":"middle-orbit","window.x":40,"window.y":30,"window.width":1280,)"
                    R"("window.height":800,"panels.model.open":false,"panels.properties.width":333}})"));
    }
    // The file holds only what was changed, nested by the key's parts.
    const std::filesystem::path file = std::filesystem::path(ray::test::fsPath(user)) / "settings.json";
    REQUIRE(std::filesystem::exists(file));
    const std::string text = readText(file);
    CHECK_THAT(text, ContainsSubstring(R"("format":"raycad.settings")"));
    CHECK_THAT(text, ContainsSubstring(R"("version":1)"));
    CHECK_THAT(text, ContainsSubstring(R"("units":{"newDocument":"metric"})"));
    CHECK_THAT(text, ContainsSubstring(R"("graphics":{"msaaSamples":2,"tier":"high"})"));
    CHECK_THAT(text, ContainsSubstring(R"("panels":{"model":{"open":false},"properties":{"width":333}})"));
    // No temporary file is left behind (writes go through a temporary file that replaces settings.json).
    CHECK(filesIn(ray::test::fsPath(user)).size() == 1);
    {
        auto app = ray::api::makeHeadlessApp(ray::api::AppOptions{});
        REQUIRE(app);
        auto store = ray::api::settingsOf(*app);
        REQUIRE(store);
        const ray::api::UserSettings s = store->get();
        CHECK(s.newDocumentUnits == UnitSystem::Metric);
        CHECK(s.tier == TierChoice::High);
        CHECK(s.theme == ray::api::ThemeMode::Light);
        CHECK(s.glass == std::optional<bool>(true));
        CHECK(s.overrides.msaaSamples == std::optional<int>(2));
        CHECK(s.navigationPreset == "middle-orbit");
        REQUIRE(s.window.has_value());
        CHECK(s.window->x == 40);
        CHECK(s.window->width == 1280);
        CHECK_FALSE(s.panels.at("model").open);
        CHECK(s.panels.at("properties").width == std::optional<int>(333));
        // settings.get reports it too.
        auto got = run(*app, "settings.get", R"({"key":"graphics.msaaSamples"})");
        REQUIRE(got);
        CHECK_THAT(got->resultJson, ContainsSubstring(R"("value":2)"));
        CHECK_THAT(got->resultJson, ContainsSubstring(R"("set":true)"));
        CHECK_THAT(got->resultJson, ContainsSubstring(R"("default":null)"));
        auto all = run(*app, "settings.get", "{}");
        REQUIRE(all);
        CHECK_THAT(all->resultJson, ContainsSubstring(R"("units.newDocument":"metric")"));
        CHECK_THAT(all->resultJson, ContainsSubstring(R"("units.decimalsInch":4)"));
    }
}

TEST_CASE("new documents take the default unit system; changing it affects the next new document only",
          "[api][settings]")
{
    TempDir dir;
    auto app = makeTestApp(dir.file("user"));
    auto first = app->newDocument("");
    REQUIRE(first);
    CHECK((*first)->document->state()->settings.units == UnitSystem::Inch);
    CHECK((*first)->document->state()->settings.decimals == 4);
    CHECK_FALSE((*first)->document->dirty());

    REQUIRE(run(*app, "settings.set", R"({"key":"units.newDocument","value":"metric"})"));
    REQUIRE(run(*app, "settings.set", R"({"key":"units.decimalsMetric","value":2})"));
    CHECK((*first)->document->state()->settings.units == UnitSystem::Inch); // the open one keeps its units

    auto second = run(*app, "file.new", "{}");
    REQUIRE(second);
    ray::api::OpenDocument* od = app->active();
    REQUIRE(od != nullptr);
    CHECK(od != *first);
    CHECK(od->document->state()->settings.units == UnitSystem::Metric);
    CHECK(od->document->state()->settings.decimals == 2);
    CHECK_FALSE(od->document->dirty());
    CHECK((*first)->document->state()->settings.units == UnitSystem::Inch);
}

TEST_CASE("a corrupt settings file falls back to the defaults and is kept aside", "[api][settings]")
{
    TempDir dir;
    const std::filesystem::path user = dir.path() / "user";
    std::filesystem::create_directories(user);
    const std::string garbage = "{\"units\": {\"newDocument\": \"metric\"";
    ray::test::writeBytes(ray::test::utf8(user / "settings.json"), garbage);
    {
        auto app = makeTestApp(ray::test::utf8(user));
        auto store = ray::api::settingsOf(*app);
        REQUIRE(store);
        CHECK(store->get().newDocumentUnits == UnitSystem::Inch);
        REQUIRE(store->loadProblem().has_value());
        CHECK_THAT(*store->loadProblem(), ContainsSubstring("default settings"));
        CHECK_THAT(*store->loadProblem(), ContainsSubstring("settings.corrupt-"));
        // The bad file is kept, untouched, under another name; settings.json is gone until something is saved.
        std::vector<std::filesystem::path> aside;
        for (const auto& f : filesIn(user)) {
            if (f.filename().string().rfind("settings.corrupt-", 0) == 0) {
                aside.push_back(f);
            }
        }
        REQUIRE(aside.size() == 1);
        CHECK(readText(aside.front()) == garbage);
        CHECK_FALSE(std::filesystem::exists(user / "settings.json"));
        // The session goes on: documents open and settings save again.
        auto od = app->newDocument("");
        REQUIRE(od);
        CHECK((*od)->document->state()->settings.units == UnitSystem::Inch);
        REQUIRE(run(*app, "settings.set", R"({"key":"units.decimalsInch","value":3})"));
        CHECK(std::filesystem::exists(user / "settings.json"));
    }
    // Not JSON at all, and JSON that is not a settings object, are handled the same way.
    for (const std::string& bad : {std::string("\x01\x02 not json"), std::string("[1,2,3]"),
                                   std::string(R"({"format":"something.else","units":{}})")}) {
        TempDir other;
        ray::test::writeBytes(other.file("settings.json"), bad);
        auto store = SettingsStore::open(ray::test::utf8(other.path()));
        CHECK(store->loadProblem().has_value());
        CHECK(store->get().decimalsInch == 4);
        CHECK_FALSE(std::filesystem::exists(other.path() / "settings.json"));
    }
}

TEST_CASE("invalid values are ignored one by one; unknown and newer members are kept", "[api][settings]")
{
    TempDir dir;
    ray::test::writeBytes(dir.file("settings.json"),
                          R"({"format":"raycad.settings","version":7,)"
                          R"("units":{"newDocument":"metric","decimalsInch":"lots"},)"
                          R"("graphics":{"msaaSamples":3,"tier":"low","futureOption":{"a":1}},)"
                          R"("plugins":{"x":true}})");
    {
        auto store = SettingsStore::open(ray::test::utf8(dir.path()));
        CHECK_FALSE(store->loadProblem().has_value()); // the file as a whole is fine
        const ray::api::UserSettings s = store->get();
        CHECK(s.newDocumentUnits == UnitSystem::Metric);
        CHECK(s.decimalsInch == 4);           // "lots" is not an integer: default
        CHECK_FALSE(s.overrides.msaaSamples); // 3 is not 0, 2, 4 or 8: default
        CHECK(s.tier == TierChoice::Low);
        REQUIRE(store->set("units.decimalsMetric", "1"));
    }
    const std::string text = readText(dir.path() / "settings.json");
    CHECK_THAT(text, ContainsSubstring(R"("futureOption":{"a":1})"));
    CHECK_THAT(text, ContainsSubstring(R"("plugins":{"x":true})"));
    CHECK_THAT(text, ContainsSubstring(R"("version":7)")); // a newer file stays marked as newer
    CHECK_THAT(text, ContainsSubstring(R"("decimalsMetric":1)"));
    CHECK_THAT(text, !ContainsSubstring("lots"));
}

TEST_CASE("settings actions validate their arguments against the key's schema", "[api][settings]")
{
    TempDir dir;
    auto app = makeTestApp(dir.file("user"));
    auto store = ray::api::settingsOf(*app);
    REQUIRE(store);
    for (const char* id : {"settings.get", "settings.set", "settings.reset"}) {
        auto info = app->actions().find(id);
        REQUIRE(info);
        CHECK(info->category == "Settings");
        CHECK_FALSE(info->changesDocument);
        CHECK_FALSE(info->changesToolpathsOrGcode);
    }

    auto bad = run(*app, "settings.set", R"({"key":"units.decimalsInch","value":12})");
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == ErrorCode::InvalidArgument);
    CHECK_THAT(bad.error().message, ContainsSubstring("/value"));
    bad = run(*app, "settings.set", R"({"key":"units.newDocument","value":"furlong"})");
    REQUIRE_FALSE(bad);
    CHECK_THAT(bad.error().message, ContainsSubstring("/value"));
    bad = run(*app, "settings.set", R"({"key":"no.such.key","value":1})");
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == ErrorCode::InvalidArgument);
    CHECK_THAT(bad.error().message, ContainsSubstring("/key"));
    bad = run(*app, "settings.set", R"({"key":"units.decimalsInch"})");
    REQUIRE_FALSE(bad);
    bad = run(*app, "settings.set", R"({"values":{"units.decimalsInch":2,"window.width":5}})");
    REQUIRE_FALSE(bad);
    CHECK_THAT(bad.error().message, ContainsSubstring("/values/window.width"));
    CHECK(store->get().decimalsInch == 4); // all or nothing
    CHECK(run(*app, "settings.set", "{}").error().code == ErrorCode::InvalidArgument);

    // Text from the palette's form: numbers and booleans as strings, "default" for "back to the default".
    REQUIRE(run(*app, "settings.set", R"({"key":"units.decimalsInch","value":"2"})"));
    CHECK(store->get().decimalsInch == 2);
    REQUIRE(run(*app, "settings.set", R"({"key":"graphics.smoothEdges","value":"true"})"));
    CHECK(store->get().overrides.smoothEdges == std::optional<bool>(true));
    REQUIRE(run(*app, "settings.set", R"({"key":"graphics.meshChordTolMm","value":"0.02"})"));
    CHECK(store->get().overrides.meshChordTolMm == std::optional<double>(0.02));
    REQUIRE(run(*app, "settings.set", R"({"key":"graphics.smoothEdges","value":"default"})"));
    CHECK_FALSE(store->get().overrides.smoothEdges.has_value());
    CHECK_FALSE(store->isSet("graphics.smoothEdges"));

    // Reset one key, a group, everything.
    REQUIRE(run(*app, "settings.set", R"({"values":{"graphics.tier":"medium","graphics.shadows":true}})"));
    auto reset = run(*app, "settings.reset", R"({"key":"graphics"})");
    REQUIRE(reset);
    CHECK_THAT(reset->resultJson, ContainsSubstring("graphics.tier"));
    CHECK_THAT(reset->resultJson, ContainsSubstring("graphics.shadows"));
    CHECK(store->get().tier == TierChoice::Auto);
    CHECK(store->get().decimalsInch == 2); // other groups untouched
    CHECK(run(*app, "settings.reset", R"({"key":"nothing"})").error().code == ErrorCode::NotFound);
    REQUIRE(run(*app, "settings.reset", R"({"key":"all"})"));
    CHECK(store->setKeys().empty());
    CHECK(store->get().decimalsInch == 4);
}

TEST_CASE("settings changes notify listeners with the keys that changed", "[api][settings]")
{
    TempDir dir;
    auto store = SettingsStore::open(dir.file("user"));
    std::vector<std::vector<std::string>> calls;
    ray::api::UserSettings last;
    const int id = store->subscribe([&](const ray::api::UserSettings& s, const std::vector<std::string>& keys) {
        calls.push_back(keys);
        last = s;
    });
    REQUIRE(id != 0);
    REQUIRE(store->set("graphics.msaaSamples", "8"));
    REQUIRE(calls.size() == 1);
    CHECK(calls[0] == std::vector<std::string>{"graphics.msaaSamples"});
    CHECK(last.overrides.msaaSamples == std::optional<int>(8));
    REQUIRE(store->set("graphics.msaaSamples", "8")); // no change: no call
    CHECK(calls.size() == 1);
    // Setting a key to its default value still counts: it is now explicit (written to the file).
    REQUIRE(store->set("units.newDocument", "\"inch\""));
    REQUIRE(calls.size() == 2);
    CHECK(calls[1] == std::vector<std::string>{"units.newDocument"});
    REQUIRE(store->setMany(R"({"window.width":900,"appearance.theme":"dark"})"));
    REQUIRE(calls.size() == 3);
    CHECK(calls[2] == std::vector<std::string>{"appearance.theme", "window.width"}); // keys() order
    store->unsubscribe(id);
    REQUIRE(store->reset(""));
    CHECK(calls.size() == 3);
}

TEST_CASE("one store per user data folder in a process", "[api][settings]")
{
    TempDir dir;
    const std::string user = dir.file("user");
    auto early = SettingsStore::open(user); // the desktop UI opens it before the app exists
    REQUIRE(early->set("units.decimalsMetric", "1"));
    auto app = makeTestApp(user);
    auto store = ray::api::settingsOf(*app);
    CHECK(store == early);
    CHECK(store->get().decimalsMetric == 1);
    TempDir other;
    CHECK(SettingsStore::open(other.file("user")) != early);
}
