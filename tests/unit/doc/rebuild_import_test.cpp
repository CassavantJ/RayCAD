// The import feature: importing through a command, the stored objects, undo and redo, a missing source file
// (task T-010).
#include "features/import_feature.h"
#include "features/import_props.h"
#include "rebuild_test_util.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using ray::BodyId;
using ray::ErrorCode;
using ray::Hash256;
using ray::JobContext;
using ray::doc::Body;
using ray::doc::BuildResult;
using ray::doc::BuildResultPtr;
using ray::doc::DocStatePtr;
using ray::doc::FeatureStatus;
using ray::doc::test::cmd;
using ray::doc::test::dataFile;
using ray::doc::test::DocEnv;
using ray::doc::test::featureId;
using ray::doc::test::reportOf;
using ray::doc::test::TempDir;
using ray::doc::test::uuid;
using ray::kernel::SubId;
using ray::kernel::SubKind;

namespace {

std::uint64_t fileImports()
{
    return ray::doc::features::importCounters().fileImports.load();
}

std::uint64_t restores()
{
    return ray::doc::features::importCounters().restores.load();
}

std::string hashOfFile(const std::string& path)
{
    JobContext ctx;
    auto hash = ray::doc::hashFile(path, ctx);
    REQUIRE(hash);
    return hash->hex();
}

// Valid shapes and element maps that name every face, edge and vertex.
void checkBody(const ray::doc::BuiltBody& body)
{
    REQUIRE_FALSE(body.shape.isNull());
    CHECK(body.shape.count(SubKind::Face) > 0);
    auto mass = ray::kernel::massProps(body.shape);
    REQUIRE(mass);
    CHECK(mass->volume > 0.0);
    const std::size_t elements =
        body.shape.count(SubKind::Face) + body.shape.count(SubKind::Edge) + body.shape.count(SubKind::Vertex);
    CHECK(body.map.size() == elements);
    for (const SubKind kind : {SubKind::Face, SubKind::Edge, SubKind::Vertex}) {
        for (std::uint32_t i = 0; i < body.shape.count(kind); ++i) {
            CHECK_FALSE(body.map.namesOf(SubId{kind, i}).empty());
        }
    }
    auto restored = ray::naming::ElementMap::deserialize(body.map.serialize());
    REQUIRE(restored);
    CHECK(restored->serialize() == body.map.serialize());
}

std::map<std::string, std::string> geometryOf(const BuildResult& build)
{
    std::map<std::string, std::string> out;
    for (const auto& [id, body] : build.bodies) {
        const auto map = body.map.serialize();
        ray::Hasher h;
        h.bytes(map);
        out[id.value.toString()] = body.shape.contentHash().hex() + "/" + h.finish().hex();
    }
    return out;
}

void copyFile(const std::string& from, const std::string& to)
{
    std::filesystem::copy_file(std::filesystem::path(std::u8string(from.begin(), from.end())),
                               std::filesystem::path(std::u8string(to.begin(), to.end())),
                               std::filesystem::copy_options::overwrite_existing);
}

std::string importPropsWithoutBodies(const std::string& path, const std::string& hash)
{
    return R"({"sourceFile":")" + path + R"(","format":"step","fileHash":")" + hash + R"("})";
}

// JSON string escaping for paths (Windows paths hold backslashes).
std::string escaped(const std::string& s)
{
    std::string out;
    for (const char c : s) {
        if (c == '\\' || c == '"') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

} // namespace

TEST_CASE("importing a STEP file through a command builds its bodies", "[doc][rebuild][import]")
{
    DocEnv env;
    const auto restoresBefore = restores();
    const auto importsBefore = fileImports();
    const DocStatePtr state = env.import(dataFile("assembly_mm.step"), 1001);
    REQUIRE(state);
    REQUIRE(env.settle());
    REQUIRE(env.doc->builtState() == state);
    const BuildResultPtr build = env.doc->build();
    REQUIRE(build);

    // One feature, four bodies, each with a Body entry in the document (name, colour, role from the file).
    REQUIRE(state->features.size() == 1);
    CHECK(state->features[0].type == "import");
    CHECK(state->features[0].name == "Import assembly_mm.step");
    REQUIRE(state->bodies.size() == 4);
    REQUIRE(build->bodies.size() == 4);
    for (const Body& body : state->bodies) {
        INFO(body.name);
        CHECK_FALSE(body.name.empty());
        CHECK(body.role == Body::Role::Part);
        REQUIRE(build->bodies.count(body.id) == 1);
        checkBody(build->bodies.at(body.id));
    }
    const auto& report = reportOf(*build, featureId(1001));
    CHECK((report.status == FeatureStatus::Ok || report.status == FeatureStatus::Warning));
    CHECK_FALSE(report.cacheHit);
    // Built from the stored objects, not by reading the file again.
    CHECK(restores() == restoresBefore + 1);
    CHECK(fileImports() == importsBefore);

    // The props name the file, its hash and every body's BREP object; the objects are in the pool.
    auto props = ray::doc::features::parseImportProps(state->features[0].propsJson, true, "test");
    REQUIRE(props);
    CHECK(props->fileHash.hex() == hashOfFile(dataFile("assembly_mm.step")));
    CHECK(props->sourceUnits == ray::UnitSystem::Metric);
    REQUIRE(props->bodies);
    CHECK(props->bodies->size() == 4);
    const std::vector<Hash256> referenced = ray::doc::referencedObjects(*state);
    CHECK(referenced.size() == 4);
    for (const auto& body : *props->bodies) {
        CHECK(env.pool->has(body.blob));
        CHECK(build->bodies.at(body.id).shape.contentHash() == body.blob);
    }

    // The import is one undo step that removes the feature and the bodies together.
    REQUIRE(env.doc->undo());
    CHECK(env.doc->state()->features.empty());
    CHECK(env.doc->state()->bodies.empty());
}

TEST_CASE("restored and re-imported assembly bodies are identical to the stored ones", "[doc][rebuild][import]")
{
    // Bodies placed by a STEP assembly change their BREP bytes on the first serialise round trip; the import
    // stores them after one, so every path gives the same bytes, hashes and names.
    std::map<std::string, std::string> restored;
    DocStatePtr state;
    {
        DocEnv env;
        state = env.import(dataFile("assembly_mm.step"), 1051);
        REQUIRE(env.settle());
        restored = geometryOf(*env.doc->build());
    }
    const auto importsBefore = fileImports();
    DocEnv env(4, state); // no stored objects: the file is imported again
    REQUIRE(env.settle());
    const BuildResultPtr build = env.doc->build();
    CHECK(fileImports() == importsBefore + 1);
    const auto& report = reportOf(*build, featureId(1051));
    CHECK(report.status == FeatureStatus::Warning);
    CHECK_THAT(report.message, ContainsSubstring("stored geometry was missing"));
    CHECK_THAT(report.message, !ContainsSubstring("differs"));
    CHECK(geometryOf(*build) == restored);
}

TEST_CASE("an IGES file imports too", "[doc][rebuild][import]")
{
    DocEnv env;
    env.import(dataFile("box_hole.igs"), 1101);
    REQUIRE(env.settle());
    const BuildResultPtr build = env.doc->build();
    REQUIRE(build);
    REQUIRE(build->bodies.size() == 1);
    checkBody(build->bodies.begin()->second);
}

TEST_CASE("undo and redo of an import restore the build from the cache without importing again",
          "[doc][rebuild][import]")
{
    DocEnv env;
    const std::string path = dataFile("box_hole.step");
    const std::string hash = hashOfFile(path);

    SECTION("an import feature without stored bodies, which imports its file")
    {
        const auto importsBefore = fileImports();
        env.run(cmd("feature.add", R"({"id":")" + uuid('f', 1201) + R"(","type":"import","name":"Import 1","props":)" +
                                       importPropsWithoutBodies(escaped(path), hash) + "}"));
        REQUIRE(env.settle());
        const BuildResultPtr first = env.doc->build();
        REQUIRE(first);
        CHECK(fileImports() == importsBefore + 1);
        REQUIRE(first->bodies.size() == 1);
        const BodyId body = ray::doc::importedBodyId(featureId(1201), 0);
        REQUIRE(first->bodies.count(body) == 1);
        checkBody(first->bodies.at(body));

        REQUIRE(env.doc->undo());
        REQUIRE(env.settle());
        CHECK(env.doc->build()->bodies.empty());
        REQUIRE(env.doc->redo());
        REQUIRE(env.settle());
        const BuildResultPtr again = env.doc->build();
        CHECK(reportOf(*again, featureId(1201)).cacheHit);
        CHECK(fileImports() == importsBefore + 1); // not imported again
        CHECK(geometryOf(*again) == geometryOf(*first));
    }

    SECTION("an import with stored bodies")
    {
        const auto restoresBefore = restores();
        env.import(path, 1202);
        REQUIRE(env.settle());
        const BuildResultPtr first = env.doc->build();
        CHECK(restores() == restoresBefore + 1);
        REQUIRE(env.doc->undo());
        REQUIRE(env.settle());
        REQUIRE(env.doc->redo());
        REQUIRE(env.settle());
        const BuildResultPtr again = env.doc->build();
        CHECK(reportOf(*again, featureId(1202)).cacheHit);
        CHECK(restores() == restoresBefore + 1);
        CHECK(geometryOf(*again) == geometryOf(*first));
    }
}

TEST_CASE("a document whose import source file is missing rebuilds from the stored objects", "[doc][rebuild][import]")
{
    TempDir dir;
    const std::string path = dir.file("part.step");
    copyFile(dataFile("box_hole.step"), path);

    // Import, then "save": the snapshot objects and the referenced BREP objects go to a store (a map here).
    std::map<Hash256, std::string> features;
    std::string manifest;
    std::map<std::string, std::string> geometry;
    std::vector<std::pair<Hash256, std::vector<std::byte>>> breps;
    {
        DocEnv env;
        env.import(path, 1301);
        REQUIRE(env.settle());
        geometry = geometryOf(*env.doc->build());
        const ray::doc::StateObjects objects = ray::doc::serializeState(*env.doc->state());
        manifest = objects.manifest;
        for (const auto& [hash, bytes] : objects.features) {
            features[hash] = bytes;
        }
        for (const Hash256& hash : ray::doc::referencedObjects(*env.doc->state())) {
            auto bytes = env.pool->object(hash);
            REQUIRE(bytes);
            breps.emplace_back(hash, std::move(*bytes));
        }
        CHECK(breps.size() == 1);
    }
    REQUIRE(std::filesystem::remove(std::filesystem::path(std::u8string(path.begin(), path.end()))));

    auto load = [&]() -> DocStatePtr {
        auto state = ray::doc::deserializeState(manifest, [&](const Hash256& h) -> ray::Result<std::string> {
            const auto it = features.find(h);
            if (it == features.end()) {
                return tl::unexpected(ray::Error{ErrorCode::NotFound, "missing", {}});
            }
            return it->second;
        });
        REQUIRE(state);
        return std::make_shared<const ray::doc::DocState>(std::move(*state));
    };

    SECTION("with the objects: the same bodies, from the objects")
    {
        auto store = ray::doc::makeObjectPool();
        for (const auto& [hash, bytes] : breps) {
            REQUIRE(*store->put(bytes) == hash);
        }
        const auto restoresBefore = restores();
        DocEnv env(4, load(), store);
        REQUIRE(env.settle());
        const BuildResultPtr build = env.doc->build();
        REQUIRE(build);
        const auto& report = build->reports.at(0);
        CHECK((report.status == FeatureStatus::Ok || report.status == FeatureStatus::Warning));
        CHECK(geometryOf(*build) == geometry);
        CHECK(restores() == restoresBefore + 1);
        for (const auto& [id, body] : build->bodies) {
            checkBody(body);
        }
    }

    SECTION("without the objects or the file: a clear error on the import feature")
    {
        DocEnv env(4, load());
        REQUIRE(env.settle());
        const BuildResultPtr build = env.doc->build();
        REQUIRE(build);
        const auto& report = build->reports.at(0);
        CHECK(report.status == FeatureStatus::Failed);
        CHECK_THAT(report.message, ContainsSubstring("stored geometry"));
        CHECK_THAT(report.message, ContainsSubstring("is missing"));
        CHECK_THAT(report.message, ContainsSubstring("part.step"));
        CHECK(build->bodies.empty());
    }

    SECTION("without the objects but with the file back: imported again, with a warning")
    {
        copyFile(dataFile("box_hole.step"), path);
        const auto importsBefore = fileImports();
        DocEnv env(4, load());
        REQUIRE(env.settle());
        const BuildResultPtr build = env.doc->build();
        REQUIRE(build);
        const auto& report = build->reports.at(0);
        CHECK(report.status == FeatureStatus::Warning);
        CHECK_THAT(report.message, ContainsSubstring("stored geometry was missing"));
        CHECK(fileImports() == importsBefore + 1);
        CHECK(geometryOf(*build) == geometry); // same file, same kernel: the same geometry and names
    }
}

TEST_CASE("an import whose file changed since it was imported fails with a clear error", "[doc][rebuild][import]")
{
    DocEnv env;
    const std::string wrongHash(64, '0');
    env.run(cmd("feature.add", R"({"id":")" + uuid('f', 1401) + R"(","type":"import","name":"Import 1","props":)" +
                                   importPropsWithoutBodies(escaped(dataFile("box_hole.step")), wrongHash) + "}"));
    REQUIRE(env.settle());
    const auto& report = reportOf(*env.doc->build(), featureId(1401));
    CHECK(report.status == FeatureStatus::Failed);
    CHECK_THAT(report.message, ContainsSubstring("changed since it was imported"));

    // Props that do not parse fail the feature with the field named.
    env.run(cmd("feature.add",
                R"({"id":")" + uuid('f', 1402) + R"(","type":"import","name":"Import 2","props":{"format":"step"}})"));
    REQUIRE(env.settle());
    CHECK(reportOf(*env.doc->build(), featureId(1402)).status == FeatureStatus::Failed);
    CHECK_THAT(reportOf(*env.doc->build(), featureId(1402)).message, ContainsSubstring("sourceFile"));
}

TEST_CASE("prepareImport rejects bad requests", "[doc][rebuild][import]")
{
    JobContext ctx;
    ray::doc::ImportRequest request;
    request.feature = featureId(1501);
    request.path = dataFile("no_such_file.step");
    CHECK(ray::doc::prepareImport(request, ctx).error().code == ErrorCode::NotFound);
    request.path = dataFile("binary.bin");
    CHECK(ray::doc::prepareImport(request, ctx).error().code == ErrorCode::InvalidArgument);
    request.path = dataFile("garbage.step");
    CHECK_FALSE(ray::doc::prepareImport(request, ctx));
    request.path = dataFile("box_hole.step");
    request.role = Body::Role::StockModel;
    CHECK(ray::doc::prepareImport(request, ctx).error().code == ErrorCode::InvalidArgument);
    request.role = Body::Role::Fixture;
    request.feature = ray::FeatureId{};
    CHECK(ray::doc::prepareImport(request, ctx).error().code == ErrorCode::InvalidArgument);

    ray::CancelSource source;
    source.cancel();
    JobContext cancelled{source.token(), {}};
    request.feature = featureId(1501);
    CHECK(ray::doc::prepareImport(request, cancelled).error().code == ErrorCode::Cancelled);

    // A fixture import marks its bodies as fixtures, and the command is replayable from its JSON form.
    auto prepared = ray::doc::prepareImport(request, ctx);
    REQUIRE(prepared);
    CHECK(prepared->command.name == "document.import");
    CHECK(prepared->command.argsJson == ray::doc::canonicalJson(prepared->command.argsJson));
    auto json = ray::doc::commandToJson(prepared->command);
    REQUIRE(json);
    auto back = ray::doc::commandFromJson(*json);
    REQUIRE(back);
    CHECK(back->argsJson == prepared->command.argsJson);
    DocEnv env;
    for (const auto& [hash, bytes] : prepared->objects) {
        REQUIRE(env.pool->put(bytes));
    }
    const DocStatePtr state = env.run(*back);
    REQUIRE(state);
    REQUIRE(state->bodies.size() == 1);
    CHECK(state->bodies[0].role == Body::Role::Fixture);
}

TEST_CASE("document.import validates its arguments and props", "[doc][rebuild][import]")
{
    DocEnv env;
    JobContext ctx;
    ray::doc::ImportRequest request;
    request.feature = featureId(1601);
    request.path = dataFile("box_hole.step");
    auto prepared = ray::doc::prepareImport(request, ctx);
    REQUIRE(prepared);
    REQUIRE(env.run(prepared->command));

    auto fails = [&](const std::string& args, const std::string& field) {
        INFO(args);
        auto result = env.doc->execute(cmd("document.import", args));
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::InvalidArgument);
        CHECK_THAT(result.error().message, ContainsSubstring("document.import"));
        CHECK_THAT(result.error().message, ContainsSubstring(field));
    };
    // The same import again: the feature id and then the body ids are taken.
    fails(prepared->command.argsJson, "already used by a feature");
    std::string args = prepared->command.argsJson;
    const std::string oldId = uuid('f', 1601);
    args.replace(args.find(oldId), oldId.size(), uuid('f', 1602));
    fails(args, "which the document has already");

    const std::string id = R"("id":")" + uuid('f', 1603) + R"(","name":"Import")";
    const std::string hash(64, 'a');
    fails("{" + id + R"(,"props":{"sourceFile":"x.step","format":"step","fileHash":")" + hash + R"(","extra":1}})",
          "'extra'");
    fails("{" + id + R"(,"props":{"sourceFile":"x.step","format":"stl","fileHash":")" + hash + R"("}})", "format");
    fails("{" + id + R"(,"props":{"sourceFile":"x.step","format":"step","fileHash":"ABC"}})", "fileHash");
    fails("{" + id + R"(,"props":{"sourceFile":"x.step","format":"step","fileHash":")" + hash +
              R"(","options":{"sewTolerance":0}}})",
          "options.sewTolerance");
    fails("{" + id + R"(,"props":{"sourceFile":"x.step","format":"step","fileHash":")" + hash + R"(","bodies":[]}})",
          "bodies");
    fails("{" + id + R"(,"props":{"sourceFile":"x.step","format":"step","fileHash":")" + hash +
              R"(","bodies":[{"id":")" + uuid('b', 1) + R"(","name":"B","color":[2,0,0,1],"blob":")" + hash +
              R"("}]}})",
          "bodies[0].color");
    fails("{" + id + "}", "props");
}

TEST_CASE("hashFile, the object pool and the object chain", "[doc][rebuild][import]")
{
    TempDir dir;
    const std::string path = dir.file("data.bin");
    std::string content(3 * 1024 * 1024 + 17, '\0');
    for (std::size_t i = 0; i < content.size(); ++i) {
        content[i] = static_cast<char>((i * 7919) % 251);
    }
    {
        std::ofstream out(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    ray::Hasher h;
    h.bytes(std::as_bytes(std::span<const char>(content.data(), content.size())));
    CHECK(hashOfFile(path) == h.finish().hex());
    JobContext ctx;
    CHECK(ray::doc::hashFile(dir.file("missing.bin"), ctx).error().code == ErrorCode::NotFound);
    CHECK(ray::doc::hashFile("", ctx).error().code == ErrorCode::InvalidArgument);

    auto a = ray::doc::makeObjectPool();
    auto b = ray::doc::makeObjectPool();
    const std::vector<std::byte> one{std::byte{1}, std::byte{2}};
    const std::vector<std::byte> two{std::byte{3}};
    const Hash256 h1 = *a->put(one);
    CHECK(*a->put(one) == h1);
    const Hash256 h2 = *b->put(two);
    CHECK(a->has(h1));
    CHECK_FALSE(a->has(h2));
    CHECK(a->hashes() == std::vector<Hash256>{h1});
    auto chain = ray::doc::makeObjectChain({a, nullptr, b});
    CHECK(*chain->object(h1) == one);
    CHECK(*chain->object(h2) == two);
    CHECK(chain->object(Hash256{}).error().code == ErrorCode::NotFound);

    // importedBodyId: a version 4 UUID that depends on the feature and the index only.
    const auto id0 = ray::doc::importedBodyId(featureId(1), 0);
    CHECK(id0 == ray::doc::importedBodyId(featureId(1), 0));
    CHECK(id0 != ray::doc::importedBodyId(featureId(1), 1));
    CHECK(id0 != ray::doc::importedBodyId(featureId(2), 0));
    CHECK(id0.value.toString()[14] == '4');
}
