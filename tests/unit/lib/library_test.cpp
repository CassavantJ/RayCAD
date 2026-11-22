// ILibrary on a plain folder: tolerant reading, atomic saves, edits by other processes, rescans and watching.
#include "fileio.h"
#include "json.h"
#include "lib_test_util.h"
#include "library_internal.h"
#include "test_dispatcher.h"

#include <ray/libraries.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>

using namespace libtest;
using ray::ErrorCode;
using ray::lib::ILibrary;

namespace {

std::unique_ptr<ILibrary> openLibrary(const std::string& root)
{
    auto library = ILibrary::open(root);
    INFO((library ? std::string() : library.error().message));
    REQUIRE(library);
    return std::move(*library);
}

bool hasIssueFor(const ILibrary& library, const std::string& file)
{
    for (const auto& issue : library.issues()) {
        if (issue.file == file) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("a missing root folder is NotFound", "[lib]")
{
    TempDir dir;
    auto library = ILibrary::open(dir.path("no such library"));
    REQUIRE_FALSE(library);
    CHECK(library.error().code == ErrorCode::NotFound);
}

TEST_CASE("every kind saves and reads back the same item", "[lib]")
{
    TempDir dir;
    auto library = openLibrary(dir.dir());
    CHECK(library->tools().empty());
    REQUIRE((*library).save(sampleTool(1)));
    REQUIRE((*library).save(sampleHolder(2)));
    REQUIRE((*library).save(sampleMaterial(3)));
    REQUIRE((*library).save(sampleMachine(4)));
    REQUIRE((*library).save(sampleTemplate(5)));
    CHECK(exists(dir.path("tools/" + fixedId(1).toString() + ".tool.json")));
    CHECK(exists(dir.path("holders/" + fixedId(2).toString() + ".holder.json")));
    CHECK(exists(dir.path("materials/" + fixedId(3).toString() + ".material.json")));
    CHECK(exists(dir.path("machines/" + fixedId(4).toString() + ".machine.json")));
    CHECK(exists(dir.path("templates/" + fixedId(5).toString() + ".optemplate.json")));

    auto reopened = openLibrary(dir.dir());
    CHECK(reopened->issues().empty());
    REQUIRE(reopened->tools().size() == 1);
    CHECK(ray::lib::toJson(reopened->tools()[0]) == ray::lib::toJson(sampleTool(1)));
    CHECK(ray::lib::toJson(reopened->holders().at(0)) == ray::lib::toJson(sampleHolder(2)));
    CHECK(ray::lib::toJson(reopened->materials().at(0)) == ray::lib::toJson(sampleMaterial(3)));
    CHECK(ray::lib::toJson(reopened->machines().at(0)) == ray::lib::toJson(sampleMachine(4)));
    CHECK(ray::lib::toJson(reopened->templates().at(0)) == ray::lib::toJson(sampleTemplate(5)));
    // The file is the canonical form laid out for people: sorted keys, two-space indentation, final newline.
    const std::string text = readFile(toolFile(dir.dir(), fixedId(1)));
    CHECK(text.rfind("{\n  \"bodyMaterial\": \"carbide\",\n", 0) == 0);
    CHECK(text.back() == '\n');
}

TEST_CASE("a malformed file becomes an issue and does not block the rest", "[lib]")
{
    TempDir dir;
    {
        auto library = openLibrary(dir.dir());
        for (int i = 1; i <= 3; ++i) {
            REQUIRE(library->save(sampleTool(i)));
        }
        REQUIRE(library->save(sampleMaterial(10)));
    }
    const std::string notJson = dir.path("tools/broken.tool.json");
    const std::string wrongType = dir.path("tools/wrong.tool.json");
    const std::string tooNew = dir.path("tools/newer.tool.json");
    const std::string badMaterial = dir.path("materials/bad.material.json");
    writeFile(notJson, "{ \"schema\": 1, \"id\": ");
    std::string text = readFile(toolFile(dir.dir(), fixedId(2)));
    // Same tool with a string diameter, and with a newer format number.
    std::string wrong = text;
    wrong.replace(wrong.find("\"diameter\": 6"), 13, "\"diameter\": \"6\"");
    wrong.replace(wrong.find(fixedId(2).toString()), 36, fixedId(7).toString());
    writeFile(wrongType, wrong);
    std::string newer = text;
    newer.replace(newer.find("\"schema\": 1"), 11, "\"schema\": 2");
    newer.replace(newer.find(fixedId(2).toString()), 36, fixedId(8).toString());
    writeFile(tooNew, newer);
    writeFile(badMaterial, "[1, 2, 3]");
    writeFile(dir.path("tools/notes.txt"), "ignored: not a tool file name");
    writeFile(dir.path("tools/.x.tool.json.1234.tmp"), "ignored: a temporary file");

    auto library = openLibrary(dir.dir());
    CHECK(library->tools().size() == 3);
    CHECK(library->materials().size() == 1);
    const auto issues = library->issues();
    REQUIRE(issues.size() == 4);
    for (const auto& issue : issues) {
        INFO(issue.file << ": " << issue.message);
        CHECK(!issue.message.empty());
    }
    CHECK(hasIssueFor(*library, notJson));
    CHECK(hasIssueFor(*library, wrongType));
    CHECK(hasIssueFor(*library, tooNew));
    CHECK(hasIssueFor(*library, badMaterial));
    // The messages say where the problem is.
    for (const auto& issue : issues) {
        if (issue.file == wrongType) {
            CHECK(issue.message == "/diameter: must be a number, not a string");
        }
        if (issue.file == tooNew) {
            CHECK(issue.message.find("newer RayCAD") != std::string::npos);
        }
    }
    // Issues are sorted by file: deterministic.
    for (std::size_t i = 1; i < issues.size(); ++i) {
        CHECK(issues[i - 1].file <= issues[i].file);
    }
}

TEST_CASE("two files with one id: the one named after the id wins, the other is an issue", "[lib]")
{
    TempDir dir;
    {
        auto library = openLibrary(dir.dir());
        REQUIRE(library->save(sampleTool(1)));
    }
    std::string copy = readFile(toolFile(dir.dir(), fixedId(1)));
    copy.replace(copy.find("Tool 1"), 6, "Copy 1");
    const std::string copyFile = dir.path("tools/a copy.tool.json"); // sorts before the uuid name
    writeFile(copyFile, copy);
    auto library = openLibrary(dir.dir());
    REQUIRE(library->tools().size() == 1);
    CHECK(library->tools()[0].name == "Tool 1");
    REQUIRE(library->issues().size() == 1);
    CHECK(library->issues()[0].file == copyFile);
}

TEST_CASE("saving keeps keys this version does not know", "[lib]")
{
    TempDir dir;
    auto library = openLibrary(dir.dir());
    REQUIRE(library->save(sampleMachine(4)));
    const std::string file = dir.path("machines/" + fixedId(4).toString() + ".machine.json");
    std::string text = readFile(file);
    text.replace(text.find("\"verified\": false"), 17, "\"verified\": false,\n  \"comment\": \"from the manual\"");
    writeFile(file, text);
    REQUIRE(library->refresh());
    auto machine = library->machines().at(0);
    machine.maxRpm = 10000;
    REQUIRE(library->save(machine));
    const std::string saved = readFile(file);
    CHECK(saved.find("\"comment\": \"from the manual\"") != std::string::npos);
    CHECK(saved.find("\"maxRpm\": 10000") != std::string::npos);
    CHECK(library->issues().empty());
}

TEST_CASE("an item that would not pass its schema is not written", "[lib]")
{
    TempDir dir;
    auto library = openLibrary(dir.dir());
    auto tool = sampleTool(1);
    tool.diameter = -1;
    auto st = library->save(tool);
    REQUIRE_FALSE(st);
    CHECK(st.error().code == ErrorCode::InvalidArgument);
    tool = sampleTool(1);
    tool.fluteLength = tool.overallLength + 1;
    CHECK_FALSE(library->save(tool));
    tool = sampleTool(1);
    tool.id = ray::ToolId{};
    CHECK_FALSE(library->save(tool));
    CHECK_FALSE(exists(dir.path("tools")));
}

TEST_CASE("an edit made on disk by another process is detected and save returns Locked", "[lib]")
{
    TempDir dir;
    auto library = openLibrary(dir.dir());
    REQUIRE(library->save(sampleTool(1)));
    const std::string file = toolFile(dir.dir(), fixedId(1));

    SECTION("an edit of the same size whose time stamp was put back")
    {
        // Same length, same modification time: only the bytes tell.
        Process edit = Process::spawnHelper({"rewrite", file, "\"Tool 1\"", "\"Tool X\"", "1"});
        REQUIRE(edit.wait() == 0);
        auto mine = sampleTool(1);
        mine.name = "Mine";
        auto st = library->save(mine);
        REQUIRE_FALSE(st);
        CHECK(st.error().code == ErrorCode::Locked);
        // The library now holds the version on disk, and the other process's edit is still there.
        CHECK(library->tools().at(0).name == "Tool X");
        CHECK(readFile(file).find("Tool X") != std::string::npos);
        // Saving again (the user chose to overwrite) succeeds.
        REQUIRE(library->save(mine));
        CHECK(readFile(file).find("\"Mine\"") != std::string::npos);
    }
    SECTION("a file deleted by someone else")
    {
        removeFile(file);
        auto st = library->save(sampleTool(1));
        REQUIRE_FALSE(st);
        CHECK(st.error().code == ErrorCode::Locked);
        CHECK(library->tools().empty());
        REQUIRE(library->save(sampleTool(1)));
    }
    SECTION("a file created by someone else for a new id")
    {
        auto other = openLibrary(dir.dir());
        auto newTool = sampleTool(2);
        REQUIRE(other->save(newTool));
        newTool.name = "Mine too";
        auto st = library->save(newTool);
        REQUIRE_FALSE(st);
        CHECK(st.error().code == ErrorCode::Locked);
        CHECK(library->tools().size() == 2);
    }
    SECTION("remove also refuses a changed file")
    {
        Process edit = Process::spawnHelper({"rewrite", file, "\"Tool 1\"", "\"Tool Y\"", "1"});
        REQUIRE(edit.wait() == 0);
        auto st = library->remove(ray::lib::ItemKind::Tool, fixedId(1));
        REQUIRE_FALSE(st);
        CHECK(st.error().code == ErrorCode::Locked);
        REQUIRE(library->remove(ray::lib::ItemKind::Tool, fixedId(1)));
        CHECK_FALSE(exists(file));
        CHECK(library->tools().empty());
        CHECK(library->remove(ray::lib::ItemKind::Tool, fixedId(1)).error().code == ErrorCode::NotFound);
        CHECK(library->remove(static_cast<ray::lib::ItemKind>(99), fixedId(1)).error().code ==
              ErrorCode::InvalidArgument);
        // Each kind is removed from its own folder.
        REQUIRE(library->save(sampleHolder(1)));
        CHECK(library->remove(ray::lib::ItemKind::Tool, fixedId(1)).error().code == ErrorCode::NotFound);
        REQUIRE(library->remove(ray::lib::ItemKind::Holder, fixedId(1)));
        CHECK(library->holders().empty());
    }
}

TEST_CASE("rescans find edits even when size and time stamp do not change", "[lib]")
{
    TempDir dir;
    auto library = openLibrary(dir.dir());
    REQUIRE(library->save(sampleTool(1)));
    const std::string file = toolFile(dir.dir(), fixedId(1));
    // Our own save is not reported as a change.
    CHECK_FALSE(library->refresh());

    SECTION("an edit within the time stamp tick of the last read is found by the next rescan")
    {
        // The file is not yet confirmed (one read since it was written), so the next rescan reads it again.
        Process edit = Process::spawnHelper({"rewrite", file, "\"Tool 1\"", "\"Tool A\"", "1"});
        REQUIRE(edit.wait() == 0);
        CHECK(library->refresh());
        CHECK(library->tools().at(0).name == "Tool A");
    }
    SECTION("a confirmed file is read again by every 12th rescan")
    {
        for (int i = 0; i < 3; ++i) {
            CHECK_FALSE(library->refresh()); // confirmed after two reads one rescan apart
        }
        Process edit = Process::spawnHelper({"rewrite", file, "\"Tool 1\"", "\"Tool B\"", "1"});
        REQUIRE(edit.wait() == 0);
        int rescans = 0;
        while (!library->refresh()) {
            ++rescans;
            REQUIRE(rescans < 12);
        }
        CHECK(library->tools().at(0).name == "Tool B");
    }
    SECTION("an ordinary edit (new time stamp) is found at once")
    {
        for (int i = 0; i < 3; ++i) {
            CHECK_FALSE(library->refresh());
        }
        Process edit = Process::spawnHelper({"rewrite", file, "\"Tool 1\"", "\"Tool C\"", "0"});
        REQUIRE(edit.wait() == 0);
        CHECK(library->refresh());
        CHECK(library->tools().at(0).name == "Tool C");
    }
    SECTION("a new file and a deleted file")
    {
        auto other = openLibrary(dir.dir());
        REQUIRE(other->save(sampleTool(2)));
        CHECK(library->refresh());
        CHECK(library->tools().size() == 2);
        REQUIRE(other->remove(ray::lib::ItemKind::Tool, fixedId(1)));
        CHECK(library->refresh());
        CHECK(library->tools().size() == 1);
    }
}

TEST_CASE("watch posts a change through the dispatcher", "[lib]")
{
    ray::lib::detail::setWatchIntervalMs(20);
    TempDir dir;
    ray::test::TestDispatcher dispatcher;
    {
        auto library = openLibrary(dir.dir());
        std::atomic<int> calls{0};
        library->watch(dispatcher, [&calls] { ++calls; });
        auto other = openLibrary(dir.dir());
        REQUIRE(other->save(sampleTool(1)));
        CHECK(dispatcher.runUntil([&calls] { return calls.load() > 0; }, std::chrono::milliseconds(10000)));
        CHECK(library->tools().size() == 1);
    } // the watch thread stops with the library
    ray::lib::detail::setWatchIntervalMs(5000);
}

TEST_CASE("post files are listed", "[lib]")
{
    TempDir dir;
    writeFile(dir.path("posts/fanuc.post.js"), "definePost({});");
    writeFile(dir.path("posts/generic-iso.post.js"), "definePost({});");
    writeFile(dir.path("posts/readme.txt"), "");
    auto library = openLibrary(dir.dir());
    const auto posts = library->postFiles();
    REQUIRE(posts.size() == 2);
    CHECK(posts[0] == dir.path("posts/fanuc.post.js"));
    CHECK(posts[1] == dir.path("posts/generic-iso.post.js"));
}

TEST_CASE("a malformed library.json is an issue", "[lib]")
{
    TempDir dir;
    writeFile(dir.path("library.json"), "{\"schema\": 1}");
    auto library = openLibrary(dir.dir());
    CHECK(hasIssueFor(*library, dir.path("library.json")));
    CHECK(library->name().empty());
    writeFile(dir.path("library.json"), "{\"schema\": 1, \"name\": \"Shop\"}");
    library->refresh();
    CHECK(library->issues().empty());
    CHECK(library->name() == "Shop"); // the display name (LibraryStack::roots)
}

TEST_CASE("a comment and unknown keys of a file survive reading and saving through the item", "[lib]")
{
    TempDir dir;
    auto library = openLibrary(dir.dir());
    auto tool = sampleTool(1);
    tool.comment = "Catalogue page 12";
    tool.extraJson = R"({"coating":"TiAlN","future":{"x":1}})";
    REQUIRE(library->save(tool));
    const std::string saved = readFile(toolFile(dir.dir(), fixedId(1)));
    CHECK(saved.find("\"coating\": \"TiAlN\"") != std::string::npos);
    CHECK(saved.find("\"comment\": \"Catalogue page 12\"") != std::string::npos);
    // Read back, the item carries both; its toJson is the file without whitespace.
    const auto read = library->tools().at(0);
    CHECK(read.comment == "Catalogue page 12");
    CHECK(read.extraJson == R"({"coating":"TiAlN","future":{"x":1}})");
    auto parsed = ray::lib::json::parse(saved);
    REQUIRE(parsed);
    CHECK(ray::lib::toJson(read) == ray::lib::json::write(*parsed));
}
