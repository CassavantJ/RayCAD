// Document snapshots: serializeState, deserializeState, stateHash.
#include "json.h"

#include <ray/document.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdlib>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>

using Catch::Matchers::ContainsSubstring;
using ray::ErrorCode;
using ray::Hash256;
using ray::Result;
using ray::Uuid;
using ray::doc::Body;
using ray::doc::DocState;
using ray::doc::Feature;
using ray::doc::Parameter;
using ray::doc::StateObjects;

namespace {

Uuid id(const char* text)
{
    const Uuid u = Uuid::fromString(text);
    REQUIRE(u.toString() == text);
    return u;
}

// A fixed snapshot with every field set. Its serialisation is pinned by the files in golden/.
DocState sample()
{
    DocState s;
    s.settings.units = ray::UnitSystem::Metric;
    s.settings.decimals = 3;
    s.settings.material = ray::MaterialId{id("6d1b3a52-6a3e-4c6f-9a55-0f2b8f3c1a01")};
    s.parameters.push_back(
        {ray::ParamId{id("0e5f0a1c-1111-4aaa-8bbb-000000000001")}, "width", "4 in", Parameter::Kind::Length, ""});
    s.parameters.push_back({ray::ParamId{id("0e5f0a1c-1111-4aaa-8bbb-000000000002")}, "angle", "width / 1 in * 30 deg",
                            Parameter::Kind::Angle, ""});
    s.parameters.push_back(
        {ray::ParamId{id("0e5f0a1c-1111-4aaa-8bbb-000000000003")}, "holes", "4", Parameter::Kind::Count, ""});
    s.features.push_back({ray::FeatureId{id("f0000000-0000-4000-8000-000000000001")}, "import", "Bracket.step",
                          R"({ "sourceFile": "bracket.step", "sourceUnits": "inch", "role": "part",
                               "blob": "00000000000000000000000000000000000000000000000000000000000000aa" })",
                          false, ""});
    s.features.push_back({ray::FeatureId{id("f0000000-0000-4000-8000-000000000002")}, "offset_face", "Offset 1",
                          R"({"faces":[],"distance":"0.5 mm"})", true, ""});
    s.features.push_back({ray::FeatureId{id("f0000000-0000-4000-8000-000000000003")}, "hole", "Hole \"M6\" \xC3\xA9",
                          R"({"standard":"M6x1","depth":"width / 2","positions":[[0,0],[12.7,-0.1]]})", false, ""});
    s.bodies.push_back({ray::BodyId{id("b0000000-0000-4000-8000-000000000001")},
                        "Bracket",
                        Body::Role::Part,
                        {0.72f, 0.74f, 0.78f, 1.0f},
                        true,
                        ""});
    s.bodies.push_back({ray::BodyId{id("b0000000-0000-4000-8000-000000000002")},
                        "Vise",
                        Body::Role::Fixture,
                        {0.3f, 0.3f, 0.32f, 0.5f},
                        false,
                        ""});
    return s;
}

using ObjectMap = std::map<Hash256, std::string>;

ObjectMap objectsOf(const StateObjects& objects)
{
    ObjectMap out;
    for (const auto& [hash, bytes] : objects.features) {
        out[hash] = bytes;
    }
    return out;
}

Result<DocState> load(const std::string& manifest, const ObjectMap& objects)
{
    return ray::doc::deserializeState(manifest, [&](const Hash256& h) -> Result<std::string> {
        auto it = objects.find(h);
        if (it == objects.end()) {
            return tl::unexpected(ray::Error{ErrorCode::NotFound, "no object " + h.hex(), {}});
        }
        return it->second;
    });
}

Hash256 blake3(const std::string& bytes)
{
    ray::Hasher h;
    h.bytes(std::as_bytes(std::span<const char>(bytes.data(), bytes.size())));
    return h.finish();
}

std::string readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void writeFile(const std::string& path, const std::string& bytes)
{
    std::ofstream out(path, std::ios::binary);
    out << bytes;
}

// Adds members to a JSON object text (re-written canonically).
std::string withMembers(const std::string& objectText, const std::string& extraMembers)
{
    std::string text = objectText;
    text.pop_back(); // '}'
    text += (text.size() > 1 ? "," : "") + extraMembers + "}";
    const std::string out = ray::doc::canonicalJson(text);
    REQUIRE_FALSE(out.empty());
    return out;
}

} // namespace

TEST_CASE("the storage form is canonical and stateHash hashes the manifest", "[doc][state]")
{
    const DocState state = sample();
    const StateObjects objects = ray::doc::serializeState(state);
    CHECK(ray::doc::canonicalJson(objects.manifest) == objects.manifest);
    REQUIRE(objects.features.size() == 3);
    for (const auto& [hash, bytes] : objects.features) {
        CHECK(ray::doc::canonicalJson(bytes) == bytes);
        CHECK(blake3(bytes) == hash);
        CHECK_THAT(objects.manifest, ContainsSubstring("\"" + hash.hex() + "\""));
    }
    CHECK(state.stateHash() == blake3(objects.manifest));
    CHECK(ray::doc::serializeState(state).manifest == objects.manifest); // repeatable
    // Props are stored canonically, and colours as short decimals.
    CHECK_THAT(objects.features[2].second,
               ContainsSubstring(R"("props":{"depth":"width / 2","positions":[[0,0],[12.7,-0.1]],"standard":"M6x1"})"));
    CHECK_THAT(objects.manifest, ContainsSubstring(R"("color":[0.72,0.74,0.78,1])"));
}

TEST_CASE("the storage form matches the golden files", "[doc][state]")
{
    // Set RAY_DOC_UPDATE_GOLDEN=1 to rewrite the files after a deliberate format change (and review the diff).
    const std::string dir = RAY_DOC_GOLDEN_DIR;
    const StateObjects objects = ray::doc::serializeState(sample());
    if (std::getenv("RAY_DOC_UPDATE_GOLDEN") != nullptr) {
        writeFile(dir + "/manifest.json", objects.manifest + "\n");
        for (std::size_t i = 0; i < objects.features.size(); ++i) {
            writeFile(dir + "/feature-" + std::to_string(i) + ".json", objects.features[i].second + "\n");
        }
    }
    CHECK(readFile(dir + "/manifest.json") == objects.manifest + "\n");
    for (std::size_t i = 0; i < objects.features.size(); ++i) {
        CHECK(readFile(dir + "/feature-" + std::to_string(i) + ".json") == objects.features[i].second + "\n");
    }
}

TEST_CASE("a snapshot round-trips with an equal hash", "[doc][state]")
{
    const DocState state = sample();
    const StateObjects objects = ray::doc::serializeState(state);
    const auto loaded = load(objects.manifest, objectsOf(objects));
    REQUIRE(loaded.has_value());
    CHECK(loaded->stateHash() == state.stateHash());
    const StateObjects again = ray::doc::serializeState(*loaded);
    CHECK(again.manifest == objects.manifest);
    CHECK(again.features == objects.features);

    CHECK(loaded->settings.units == ray::UnitSystem::Metric);
    CHECK(loaded->settings.decimals == 3);
    CHECK(loaded->settings.material == state.settings.material);
    REQUIRE(loaded->parameters.size() == 3);
    CHECK(loaded->parameters[1].name == "angle");
    CHECK(loaded->parameters[1].expression == "width / 1 in * 30 deg");
    CHECK(loaded->parameters[1].kind == Parameter::Kind::Angle);
    CHECK(loaded->parameters[2].kind == Parameter::Kind::Count);
    REQUIRE(loaded->features.size() == 3);
    CHECK(loaded->features[0].type == "import");
    CHECK(loaded->features[1].suppressed);
    CHECK(loaded->features[2].name == state.features[2].name);
    CHECK(loaded->features[2].propsJson == ray::doc::canonicalJson(state.features[2].propsJson));
    REQUIRE(loaded->bodies.size() == 2);
    CHECK(loaded->bodies[0].color == state.bodies[0].color); // exact floats
    CHECK(loaded->bodies[1].color == state.bodies[1].color);
    CHECK(loaded->bodies[1].role == Body::Role::Fixture);
    CHECK_FALSE(loaded->bodies[1].visible);
    for (const auto* p : {&loaded->settings.extraJson, &loaded->extraJson, &loaded->features[0].extraJson}) {
        CHECK(p->empty());
    }

    // The parameters of the loaded snapshot evaluate.
    const auto values = ray::doc::evaluateParameters(loaded->parameters);
    REQUIRE(values.has_value());
    CHECK(values->at("width") == 101.6);
    CHECK(values->at("holes") == 4);
}

TEST_CASE("an empty snapshot round-trips", "[doc][state]")
{
    const DocState empty;
    const StateObjects objects = ray::doc::serializeState(empty);
    CHECK(
        objects.manifest ==
        R"({"bodies":[],"features":[],"parameters":[],"schema":1,"settings":{"decimals":4,"material":null,"units":"inch"}})");
    const auto loaded = load(objects.manifest, {});
    REQUIRE(loaded.has_value());
    CHECK(loaded->stateHash() == empty.stateHash());
    CHECK(loaded->settings.material == ray::MaterialId{});
}

TEST_CASE("every change shows in the hash", "[doc][state]")
{
    const DocState base = sample();
    const Hash256 h = base.stateHash();
    auto changed = [&](auto edit) {
        DocState s = sample();
        edit(s);
        return s.stateHash() != h;
    };
    CHECK(changed([](DocState& s) { s.settings.decimals = 4; }));
    CHECK(changed([](DocState& s) { s.parameters[0].expression = "4.0 in"; }));
    CHECK(changed([](DocState& s) { s.features[1].suppressed = false; }));
    CHECK(changed([](DocState& s) { s.features[2].propsJson = R"({"standard":"M8x1.25"})"; }));
    CHECK(changed([](DocState& s) { std::swap(s.features[0], s.features[1]); }));
    CHECK(changed([](DocState& s) { s.bodies[0].color[3] = 0.99f; }));
    CHECK(changed([](DocState& s) { s.bodies.pop_back(); }));
    // Formatting of the props text does not.
    CHECK_FALSE(
        changed([](DocState& s) { s.features[2].propsJson = ray::doc::canonicalJson(s.features[2].propsJson); }));
    CHECK_FALSE(changed(
        [](DocState& s) { s.features[1].propsJson = "{\n  \"distance\" : \"0.5 mm\",\n  \"faces\" : [ ]\n}"; }));
}

TEST_CASE("unknown JSON keys survive a round trip", "[doc][state]")
{
    const StateObjects objects = ray::doc::serializeState(sample());
    ObjectMap store = objectsOf(objects);

    // A newer version's feature object with an extra key, stored under its own hash.
    const std::string feature = withMembers(objects.features[1].second, R"("futureFlag":{"z":[1,2.5],"a":"x"})");
    const Hash256 featureHash = blake3(feature);
    store[featureHash] = feature;

    // Rebuild the manifest text with extra keys at every level and the new feature hash.
    std::string text = objects.manifest;
    const std::string oldHash = "\"" + objects.features[1].first.hex() + "\"";
    text.replace(text.find(oldHash), oldHash.size(), "\"" + featureHash.hex() + "\"");
    auto insertAfter = [&](const std::string& marker, const std::string& members) {
        const std::size_t at = text.find(marker);
        REQUIRE(at != std::string::npos);
        text.insert(at + marker.size(), members + ",");
    };
    insertAfter(R"("settings":{)", R"("tolerance":0.001)");
    insertAfter(R"({"expression":"4 in",)", R"("comment":"overall width")");
    insertAfter(R"({"color":[0.72,0.74,0.78,1],)", R"("opacityMode":"glass","tags":["a","b"])");
    text = withMembers(text, R"("cam":")" + std::string(64, 'c') + R"(","embedded":{"tools":{}},"zz":null)");

    const auto loaded = load(text, store);
    REQUIRE(loaded.has_value());
    CHECK(loaded->settings.extraJson == R"({"tolerance":0.001})");
    CHECK(loaded->parameters[0].extraJson == R"({"comment":"overall width"})");
    CHECK(loaded->bodies[0].extraJson == R"({"opacityMode":"glass","tags":["a","b"]})");
    CHECK(loaded->features[1].extraJson == R"({"futureFlag":{"a":"x","z":[1,2.5]}})");
    CHECK(loaded->extraJson == R"({"cam":")" + std::string(64, 'c') + R"(","embedded":{"tools":{}},"zz":null})");

    const StateObjects again = ray::doc::serializeState(*loaded);
    CHECK(again.manifest == text);
    REQUIRE(again.features.size() == 3);
    CHECK(again.features[1].second == feature);
    CHECK(again.features[1].first == featureHash);
    CHECK(loaded->stateHash() == blake3(text));

    // A known key in extraJson does not override the real field.
    DocState clash = sample();
    clash.bodies[0].extraJson = R"({"name":"Other","x":1})";
    CHECK_THAT(ray::doc::serializeState(clash).manifest,
               ContainsSubstring(R"("name":"Bracket","role":"part","visible":true,"x":1)"));
}

TEST_CASE("bad documents are refused with a message", "[doc][state]")
{
    const StateObjects objects = ray::doc::serializeState(sample());
    const ObjectMap store = objectsOf(objects);
    auto expectError = [&](const std::string& manifest, ErrorCode code, const std::string& part,
                           const ObjectMap& objectStore) {
        const auto loaded = load(manifest, objectStore);
        INFO(manifest);
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error().code == code);
        CHECK_THAT(loaded.error().message, ContainsSubstring(part));
    };
    auto replaced = [&](const std::string& from, const std::string& to) {
        std::string text = objects.manifest;
        const std::size_t at = text.find(from);
        REQUIRE(at != std::string::npos);
        text.replace(at, from.size(), to);
        return text;
    };
    expectError(replaced(R"("schema":1)", R"("schema":2)"), ErrorCode::FormatTooNew, "newer RayCAD", store);
    expectError(replaced(R"("schema":1)", R"("schema":0)"), ErrorCode::FormatError, "unknown schema", store);
    expectError(replaced(R"("schema":1)", R"("schema":1.5)"), ErrorCode::FormatError, "whole number", store);
    expectError(replaced(R"("schema":1,)", ""), ErrorCode::FormatError, "missing key 'schema'", store);
    expectError(replaced(R"("units":"metric")", R"("units":"furlong")"), ErrorCode::FormatError,
                "'units' must be one of inch, metric", store);
    expectError(replaced(R"("decimals":3)", R"("decimals":"3")"), ErrorCode::FormatError, "'decimals' must be a number",
                store);
    expectError(replaced(R"("kind":"length")", R"("kind":"volume")"), ErrorCode::FormatError, "parameters[0]", store);
    expectError(replaced(R"("role":"part")", R"("role":"tool")"), ErrorCode::FormatError, "bodies[0]", store);
    expectError(replaced(R"([0.72,0.74,0.78,1])", R"([0.72,0.74,0.78])"), ErrorCode::FormatError, "4 components",
                store);
    expectError(replaced(R"([0.72,0.74,0.78,1])", R"([0.72,0.74,0.78,1e39])"), ErrorCode::FormatError, "out of range",
                store);
    expectError(replaced(R"(0e5f0a1c-1111-4aaa-8bbb-000000000001)", R"(0E5F0A1C-1111-4AAA-8BBB-000000000001)"),
                ErrorCode::FormatError, "lower-case UUID", store);
    expectError(replaced(R"(0e5f0a1c-1111-4aaa-8bbb-000000000002)", R"(0e5f0a1c-1111-4aaa-8bbb-000000000001)"),
                ErrorCode::FormatError, "duplicate id", store);
    expectError(replaced(objects.features[0].first.hex(), std::string(63, 'a') + "G"), ErrorCode::FormatError,
                "64 lower-case hex", store);
    expectError("[1,2]", ErrorCode::FormatError, "must be an object", store);
    expectError("{\"schema\":1", ErrorCode::FormatError, "Invalid JSON", store);
    // A feature object that is missing passes the store's error through; one that does not match its hash fails.
    ObjectMap missing = store;
    missing.erase(objects.features[2].first);
    expectError(objects.manifest, ErrorCode::NotFound, "no object", missing);
    ObjectMap tampered = store;
    tampered[objects.features[2].first] += " ";
    expectError(objects.manifest, ErrorCode::FormatError, "does not match its hash", tampered);
    // A broken feature object stored under its own (correct) hash.
    const std::string badFeature = R"({"id":"f0000000-0000-4000-8000-000000000009","schema":1})";
    ObjectMap withBad = store;
    withBad[blake3(badFeature)] = badFeature;
    expectError(replaced(objects.features[0].first.hex(), blake3(badFeature).hex()), ErrorCode::FormatError,
                "missing key", withBad);
    const std::string newerFeature = R"({"id":"f0000000-0000-4000-8000-000000000009","name":"x","props":{},)"
                                     R"("schema":7,"suppressed":false,"type":"x"})";
    withBad[blake3(newerFeature)] = newerFeature;
    expectError(replaced(objects.features[0].first.hex(), blake3(newerFeature).hex()), ErrorCode::FormatTooNew,
                "format 7", withBad);
}

TEST_CASE("fuzzed documents never crash and fail with FormatError", "[doc][state]")
{
    const StateObjects objects = ray::doc::serializeState(sample());
    const ObjectMap store = objectsOf(objects);
    std::mt19937 rng(1118u);
    auto check = [&](const std::string& manifest, const ObjectMap& objectStore) {
        const auto loaded = load(manifest, objectStore);
        if (loaded) {
            // Whatever loads writes back canonically and loads again to the same hash.
            const StateObjects again = ray::doc::serializeState(*loaded);
            const auto reloaded = load(again.manifest, objectsOf(again));
            REQUIRE(reloaded.has_value());
            CHECK(reloaded->stateHash() == loaded->stateHash());
        } else {
            const ErrorCode code = loaded.error().code;
            CHECK((code == ErrorCode::FormatError || code == ErrorCode::FormatTooNew || code == ErrorCode::NotFound));
        }
    };
    auto mutate = [&](std::string text) {
        const int flips = 1 + static_cast<int>(rng() % 3);
        for (int k = 0; k < flips && !text.empty(); ++k) {
            const std::size_t at = rng() % text.size();
            switch (rng() % 3) {
            case 0:
                text[at] = static_cast<char>(text[at] ^ (1 << (rng() % 8)));
                break;
            case 1:
                text[at] = static_cast<char>(rng() % 256);
                break;
            default:
                text.erase(at, 1 + rng() % 3);
                break;
            }
        }
        return text;
    };
    for (std::size_t n = 0; n <= objects.manifest.size(); ++n) {
        check(objects.manifest.substr(0, n), store);
    }
    for (int i = 0; i < 3000; ++i) {
        check(mutate(objects.manifest), store);
    }
    // Mutated feature objects, stored under their new hash so they reach the feature reader.
    for (int i = 0; i < 1500; ++i) {
        const std::size_t which = rng() % objects.features.size();
        const std::string feature = mutate(objects.features[which].second);
        ObjectMap mutated = store;
        mutated[blake3(feature)] = feature;
        std::string manifest = objects.manifest;
        const std::string oldHash = objects.features[which].first.hex();
        manifest.replace(manifest.find(oldHash), oldHash.size(), blake3(feature).hex());
        check(manifest, mutated);
    }
}

TEST_CASE("snapshots share CAM state and copy cheaply", "[doc][state]")
{
    // DocStatePtr is shared and immutable; a command copies the struct and changes what it needs.
    auto first = std::make_shared<const DocState>(sample());
    DocState edit = *first;
    edit.parameters[0].expression = "5 in";
    const ray::doc::DocStatePtr second = std::make_shared<const DocState>(std::move(edit));
    CHECK(second->cam == first->cam);
    CHECK(first->parameters[0].expression == "4 in");
    CHECK(first->stateHash() != second->stateHash());
    // Unchanged features keep their object hashes, so a store writes only the manifest again.
    const auto a = ray::doc::serializeState(*first);
    const auto b = ray::doc::serializeState(*second);
    CHECK(a.features == b.features);
    CHECK(a.manifest != b.manifest);
}
