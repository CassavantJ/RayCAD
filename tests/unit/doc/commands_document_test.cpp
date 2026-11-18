// IDocument: undo and redo, failed commands, previews, change listeners, dirty state and the JSON form of commands.
#include "commands_test_util.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using ray::ErrorCode;
using ray::doc::Command;
using ray::doc::DocStatePtr;
using ray::doc::test::cmd;
using ray::doc::test::DocFixture;
using ray::doc::test::featureAddArgs;
using ray::doc::test::hashOf;
using ray::doc::test::uuid;

namespace {

Command addFeature(std::uint64_t n)
{
    return cmd("feature.add", featureAddArgs(n), "Add Extrude " + std::to_string(n));
}

Command renameFeature(std::uint64_t n, const std::string& name)
{
    return cmd("feature.edit", R"({"id":")" + uuid('f', n) + R"(","name":")" + name + R"("})", "Rename");
}

struct Snapshot {
    DocStatePtr state;
    std::size_t undo, redo;
    bool canUndo, canRedo, dirty;
};

Snapshot snapshotOf(const ray::doc::Document& doc)
{
    return {doc.state(), doc.undoCount(), doc.redoCount(), doc.canUndo(), doc.canRedo(), doc.dirty()};
}

void unchanged(const ray::doc::Document& doc, const Snapshot& s)
{
    CHECK(doc.state() == s.state);
    CHECK(doc.undoCount() == s.undo);
    CHECK(doc.redoCount() == s.redo);
    CHECK(doc.canUndo() == s.canUndo);
    CHECK(doc.canRedo() == s.canRedo);
    CHECK(doc.dirty() == s.dirty);
}

// A handler that throws, to prove nothing escapes the document.
class ThrowingHandler final : public ray::doc::ICommandHandler {
public:
    std::string_view name() const override { return "test.throw"; }
    std::string_view argsSchemaJson() const override { return "{}"; }
    bool changesToolpathsOrGcode() const override { return false; }
    ray::Result<DocStatePtr> apply(DocStatePtr, std::string_view) const override
    {
        throw std::runtime_error("handler failure");
    }
};

class NullHandler final : public ray::doc::ICommandHandler {
public:
    std::string_view name() const override { return "test.null"; }
    std::string_view argsSchemaJson() const override { return "{}"; }
    bool changesToolpathsOrGcode() const override { return false; }
    ray::Result<DocStatePtr> apply(DocStatePtr, std::string_view) const override { return DocStatePtr(); }
};

} // namespace

TEST_CASE("a new document is empty, clean and has nothing to undo", "[doc][document]")
{
    DocFixture f;
    REQUIRE(f.doc);
    REQUIRE(f.doc->state());
    CHECK(hashOf(f.doc->state()) == ray::doc::DocState{}.stateHash().hex());
    CHECK_FALSE(f.doc->canUndo());
    CHECK_FALSE(f.doc->canRedo());
    CHECK_FALSE(f.doc->dirty());
    CHECK(f.doc->build() == nullptr);
    CHECK_FALSE(f.doc->undoCommand());
    CHECK_FALSE(f.doc->redoCommand());
    CHECK(f.doc->undo().error().code == ErrorCode::InvalidArgument);
    CHECK(f.doc->redo().error().code == ErrorCode::InvalidArgument);
    CHECK(f.ui.posted() == 0);
}

TEST_CASE("undo and redo swap between the same snapshots", "[doc][document]")
{
    DocFixture f;
    const DocStatePtr s0 = f.doc->state();
    const DocStatePtr s1 = f.run(addFeature(1));
    const DocStatePtr s2 = f.run(addFeature(2));
    const DocStatePtr s3 = f.run(renameFeature(1, "Base"));
    CHECK(f.doc->undoCount() == 3);
    CHECK(f.doc->undoCommand()->label == "Rename");

    REQUIRE(f.doc->undo());
    CHECK(f.doc->state() == s2);
    CHECK(f.doc->redoCommand()->label == "Rename");
    REQUIRE(f.doc->undo());
    REQUIRE(f.doc->undo());
    CHECK(f.doc->state() == s0);
    CHECK_FALSE(f.doc->canUndo());
    CHECK(f.doc->redoCount() == 3);
    REQUIRE(f.doc->redo());
    CHECK(f.doc->state() == s1);
    REQUIRE(f.doc->redo());
    REQUIRE(f.doc->redo());
    CHECK(f.doc->state() == s3);
    CHECK_FALSE(f.doc->canRedo());

    // Snapshots are shared, never copied, and earlier ones are never modified.
    CHECK(s0->features.empty());
    CHECK(s1->features.size() == 1);
    CHECK(s2->features[0].name == "Extrude 1");
    CHECK(s3->features[0].name == "Base");
}

TEST_CASE("a new command clears the redo stack", "[doc][document]")
{
    DocFixture f;
    f.run(addFeature(1));
    f.run(addFeature(2));
    f.run(addFeature(3));
    REQUIRE(f.doc->undo());
    REQUIRE(f.doc->undo());
    CHECK(f.doc->redoCount() == 2);
    const DocStatePtr s = f.run(addFeature(4));
    CHECK_FALSE(f.doc->canRedo());
    CHECK(f.doc->redoCount() == 0);
    CHECK(f.doc->redo().error().code == ErrorCode::InvalidArgument);
    CHECK(f.doc->state() == s);
    CHECK(f.doc->undoCount() == 2);
    REQUIRE(s->features.size() == 2);
    CHECK(s->features[1].id.value.toString() == uuid('f', 4));
}

TEST_CASE("a failing command leaves the state and both stacks untouched", "[doc][document]")
{
    DocFixture f;
    f.run(addFeature(1));
    f.run(addFeature(2));
    f.run(addFeature(3));
    REQUIRE(f.doc->undo()); // one entry on each stack
    f.ui.runPending();
    const Snapshot before = snapshotOf(*f.doc);
    const auto posted = f.ui.posted();
    int calls = 0;
    f.doc->subscribe([&](DocStatePtr, ray::doc::BuildResultPtr) { ++calls; });

    for (const Command& bad :
         {addFeature(1), renameFeature(42, "X"), cmd("feature.remove", "{}"), cmd("feature.add", "not json"),
          cmd("no.such.command", "{}"), cmd("feature.reorder", R"({"id":")" + uuid('f', 1) + R"(","index":5})")}) {
        INFO(bad.name << " " << bad.argsJson);
        auto result = f.doc->execute(bad);
        REQUIRE_FALSE(result);
        CHECK_FALSE(result.error().message.empty());
        unchanged(*f.doc, before);
    }
    CHECK(f.ui.posted() == posted);
    f.ui.runPending();
    CHECK(calls == 0);
    // The redo entry survived: redo still works.
    REQUIRE(f.doc->redo());
    CHECK(f.doc->state()->features.size() == 3);
}

TEST_CASE("a handler that throws or returns nothing is reported, not propagated", "[doc][document]")
{
    auto registry = std::make_shared<ray::doc::CommandRegistry>();
    REQUIRE(ray::doc::registerBuiltinCommands(*registry));
    REQUIRE(registry->add(std::make_unique<ThrowingHandler>()));
    REQUIRE(registry->add(std::make_unique<NullHandler>()));
    ray::test::TestDispatcher ui;
    auto doc = ray::doc::makeDocument(ui, registry);
    REQUIRE(doc);
    REQUIRE(doc->execute(addFeature(1)));
    const Snapshot before = snapshotOf(*doc);
    for (const char* name : {"test.throw", "test.null"}) {
        auto result = doc->execute(cmd(name, "{}"));
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::Internal);
        CHECK_THAT(result.error().message, ContainsSubstring(name));
        unchanged(*doc, before);
    }
    doc->beginPreview(cmd("test.throw", "{}"));
    CHECK(doc->previewState() == nullptr);
    REQUIRE(doc->previewError());
    CHECK(doc->previewError()->code == ErrorCode::Internal);
    unchanged(*doc, before);
}

TEST_CASE("beginPreview never changes state() or the undo stack", "[doc][document][preview]")
{
    DocFixture f;
    f.run(addFeature(1));
    f.run(addFeature(2));
    REQUIRE(f.doc->undo()); // something on the redo stack too
    f.ui.runPending();
    int calls = 0;
    f.doc->subscribe([&](DocStatePtr, ray::doc::BuildResultPtr) { ++calls; });
    const Snapshot before = snapshotOf(*f.doc);
    const auto posted = f.ui.posted();

    CHECK(f.doc->previewState() == nullptr);
    CHECK_FALSE(f.doc->previewError());

    SECTION("a preview shows the command applied off to the side; cancel drops it")
    {
        const Command edit = renameFeature(1, "Previewed");
        f.doc->beginPreview(edit);
        unchanged(*f.doc, before);
        const DocStatePtr preview = f.doc->previewState();
        REQUIRE(preview);
        CHECK(preview != before.state);
        CHECK(preview->features[0].name == "Previewed");
        CHECK(before.state->features[0].name == "Extrude 1");
        CHECK_FALSE(f.doc->previewError());

        // A second preview replaces the first, still without touching the document.
        f.doc->beginPreview(renameFeature(1, "Second"));
        unchanged(*f.doc, before);
        REQUIRE(f.doc->previewState());
        CHECK(f.doc->previewState()->features[0].name == "Second");

        f.doc->cancelPreview();
        CHECK(f.doc->previewState() == nullptr);
        CHECK_FALSE(f.doc->previewError());
        unchanged(*f.doc, before);
        f.doc->cancelPreview(); // cancelling with no preview is harmless
        unchanged(*f.doc, before);
        CHECK(f.ui.posted() == posted);
        f.ui.runPending();
        CHECK(calls == 0);
    }

    SECTION("committing a preview is executing its command: same snapshot, one undo entry, preview ended")
    {
        const Command edit = renameFeature(1, "Committed");
        f.doc->beginPreview(edit);
        const std::string previewHash = hashOf(f.doc->previewState());
        const auto committed = f.run(edit);
        REQUIRE(committed);
        CHECK(hashOf(committed) == previewHash);
        CHECK(f.doc->state() == committed);
        CHECK(f.doc->undoCount() == before.undo + 1);
        CHECK(f.doc->redoCount() == 0); // a new command, so redo is cleared as usual
        CHECK(f.doc->previewState() == nullptr);
        f.ui.runPending();
        CHECK(calls == 1);
        REQUIRE(f.doc->undo());
        CHECK(f.doc->state() == before.state);
    }

    SECTION("a preview of a failing command reports the error and changes nothing")
    {
        f.doc->beginPreview(renameFeature(99, "X"));
        unchanged(*f.doc, before);
        CHECK(f.doc->previewState() == nullptr);
        REQUIRE(f.doc->previewError());
        CHECK(f.doc->previewError()->code == ErrorCode::InvalidArgument);
        CHECK_THAT(f.doc->previewError()->message, ContainsSubstring("'id'"));
        f.doc->beginPreview(cmd("nope", "{}"));
        REQUIRE(f.doc->previewError());
        CHECK(f.doc->previewError()->code == ErrorCode::NotFound);
        f.doc->cancelPreview();
        CHECK_FALSE(f.doc->previewError());
        unchanged(*f.doc, before);
    }

    SECTION("undo, redo and other commands end a preview, which was made from an older snapshot")
    {
        f.doc->beginPreview(renameFeature(1, "P"));
        REQUIRE(f.doc->previewState());
        REQUIRE(f.doc->redo());
        CHECK(f.doc->previewState() == nullptr);
        f.doc->beginPreview(renameFeature(1, "P"));
        REQUIRE(f.doc->undo());
        CHECK(f.doc->previewState() == nullptr);
        f.doc->beginPreview(renameFeature(1, "P"));
        f.run(addFeature(7));
        CHECK(f.doc->previewState() == nullptr);
        // A failing command does not end it.
        f.doc->beginPreview(renameFeature(1, "P"));
        CHECK_FALSE(f.doc->execute(addFeature(7)));
        CHECK(f.doc->previewState() != nullptr);
    }
}

TEST_CASE("listeners fire on the dispatcher thread, exactly once per change", "[doc][document][listeners]")
{
    DocFixture f;
    // Listeners only record; the checks run on the test thread (Catch2 assertions are not thread-safe).
    struct Call {
        std::thread::id thread;
        DocStatePtr state;
        ray::doc::BuildResultPtr build;
    };
    std::vector<Call> first, second;
    const int a = f.doc->subscribe([&](DocStatePtr s, ray::doc::BuildResultPtr build) {
        first.push_back({std::this_thread::get_id(), std::move(s), std::move(build)});
    });
    const int b = f.doc->subscribe([&](DocStatePtr s, ray::doc::BuildResultPtr build) {
        second.push_back({std::this_thread::get_id(), std::move(s), std::move(build)});
    });
    CHECK(a != 0);
    CHECK(b != 0);
    CHECK(a != b);
    CHECK(f.doc->subscribe(nullptr) == 0);

    std::vector<DocStatePtr> expected;
    expected.push_back(f.run(addFeature(1)));
    expected.push_back(f.run(addFeature(2)));
    expected.push_back(f.run(renameFeature(2, "Two")));
    REQUIRE(f.doc->undo());
    expected.push_back(f.doc->state());
    REQUIRE(f.doc->undo());
    expected.push_back(f.doc->state());
    REQUIRE(f.doc->redo());
    expected.push_back(f.doc->state());
    // Not changes: a failed command and a preview.
    CHECK_FALSE(f.doc->execute(addFeature(1)));
    f.doc->beginPreview(addFeature(9));
    f.doc->cancelPreview();

    // Nothing is called synchronously, only through the dispatcher: one post per change.
    CHECK(first.empty());
    CHECK(second.empty());
    CHECK(f.ui.posted() == expected.size());

    std::thread::id uiThread;
    std::thread ui([&] {
        uiThread = std::this_thread::get_id();
        f.ui.runPending();
    });
    ui.join();

    REQUIRE(first.size() == expected.size());
    REQUIRE(second.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        INFO("change " << i);
        CHECK(first[i].thread == uiThread);
        CHECK(second[i].thread == uiThread);
        CHECK(first[i].state == expected[i]);
        CHECK(second[i].state == expected[i]);
        CHECK(first[i].build == nullptr); // no rebuilder until T-010
    }
    CHECK(uiThread != std::this_thread::get_id());

    // Draining again calls nobody: each change is delivered once.
    CHECK(f.ui.runPending() == 0);
    CHECK(first.size() == expected.size());

    SECTION("an unsubscribed listener is not called, even for changes already posted")
    {
        f.run(renameFeature(1, "One"));
        f.doc->unsubscribe(a);
        f.ui.runPending();
        CHECK(first.size() == expected.size());
        CHECK(second.size() == expected.size() + 1);
        f.doc->unsubscribe(a);     // twice is harmless
        f.doc->unsubscribe(12345); // unknown is harmless
    }

    SECTION("a listener subscribed after a change is not called for it")
    {
        f.run(renameFeature(1, "One"));
        int late = 0;
        f.doc->subscribe([&](DocStatePtr, ray::doc::BuildResultPtr) { ++late; });
        f.ui.runPending();
        CHECK(late == 0);
        f.run(renameFeature(1, "Uno"));
        f.ui.runPending();
        CHECK(late == 1);
    }

    SECTION("a listener may change the document; that change gets its own notification")
    {
        bool done = false;
        f.doc->subscribe([&](DocStatePtr, ray::doc::BuildResultPtr) {
            if (!done) {
                done = true;
                CHECK(f.doc->execute(renameFeature(1, "From listener")));
            }
        });
        f.run(renameFeature(1, "One"));
        f.ui.runPending();
        CHECK(f.doc->state()->features[0].name == "From listener");
        CHECK(first.size() == expected.size() + 2);
        CHECK(first.back().state == f.doc->state());
    }

    SECTION("a listener that throws does not stop the others")
    {
        f.doc->unsubscribe(a);
        f.doc->subscribe([](DocStatePtr, ray::doc::BuildResultPtr) { throw std::runtime_error("listener failure"); });
        int after = 0;
        f.doc->subscribe([&](DocStatePtr, ray::doc::BuildResultPtr) { ++after; });
        f.run(renameFeature(1, "One"));
        CHECK_NOTHROW(f.ui.runPending());
        CHECK(after == 1);
        CHECK(second.size() == expected.size() + 1);
    }
}

TEST_CASE("notifications that run after the document is gone do nothing", "[doc][document][listeners]")
{
    ray::test::TestDispatcher ui;
    int calls = 0;
    {
        auto doc = ray::doc::makeDocument(ui);
        REQUIRE(doc);
        doc->subscribe([&](DocStatePtr, ray::doc::BuildResultPtr) { ++calls; });
        REQUIRE(doc->execute(addFeature(1)));
        REQUIRE(doc->undo());
    }
    CHECK(ui.posted() == 2);
    CHECK(ui.runPending() == 2);
    CHECK(calls == 0);
}

TEST_CASE("dirty follows the snapshot that was current at markClean", "[doc][document]")
{
    DocFixture f;
    CHECK_FALSE(f.doc->dirty());
    f.run(addFeature(1));
    CHECK(f.doc->dirty());
    REQUIRE(f.doc->undo());
    CHECK_FALSE(f.doc->dirty());
    REQUIRE(f.doc->redo());
    CHECK(f.doc->dirty());
    f.doc->markClean();
    CHECK_FALSE(f.doc->dirty());
    f.run(addFeature(2));
    CHECK(f.doc->dirty());
    REQUIRE(f.doc->undo());
    CHECK_FALSE(f.doc->dirty());
    REQUIRE(f.doc->undo());
    CHECK(f.doc->dirty());
}

TEST_CASE("commands round-trip through canonical JSON", "[doc][commands][json]")
{
    const Command c =
        cmd("feature.add", R"({ "type":"hole", "id":")" + uuid('f', 1) + R"(", "name":"H", "props":{} })", "Add \"H\"");
    auto text = ray::doc::commandToJson(c);
    REQUIRE(text);
    CHECK(*text ==
          R"({"args":{"id":")" + uuid('f', 1) +
              R"(","name":"H","props":{},"type":"hole"},"label":"Add \"H\"","name":"feature.add","schema":1})");
    CHECK(ray::doc::canonicalJson(*text) == *text);
    auto back = ray::doc::commandFromJson(*text);
    REQUIRE(back);
    CHECK(back->name == c.name);
    CHECK(back->label == c.label);
    CHECK(back->argsJson == ray::doc::canonicalJson(c.argsJson));
    CHECK(ray::doc::commandToJson(*back).value() == *text);

    auto log = ray::doc::commandLogToJson({c, cmd("feature.remove", R"({"id":")" + uuid('f', 1) + R"("})")});
    REQUIRE(log);
    auto commands = ray::doc::commandLogFromJson(*log);
    REQUIRE(commands);
    REQUIRE(commands->size() == 2);
    CHECK((*commands)[1].name == "feature.remove");
    CHECK((*commands)[1].label.empty());
    CHECK(ray::doc::commandLogFromJson("[]").value().empty());

    CHECK(ray::doc::commandToJson(cmd("x", "[1]")).error().code == ErrorCode::InvalidArgument);
    CHECK(ray::doc::commandToJson(cmd("x", "nope")).error().code == ErrorCode::InvalidArgument);
    CHECK(ray::doc::commandLogToJson({cmd("x", "nope")}).error().code == ErrorCode::InvalidArgument);
    for (const char* bad :
         {"", "[]", "{}", R"({"args":{},"label":"","name":"x"})", R"({"args":{},"label":"","name":"","schema":1})",
          R"({"args":"{}","label":"","name":"x","schema":1})", R"({"args":{},"label":3,"name":"x","schema":1})",
          R"({"args":{},"label":"","name":"x","schema":0})",
          R"({"args":{},"label":"","name":"x","schema":1,"extra":true})"}) {
        INFO(bad);
        auto result = ray::doc::commandFromJson(bad);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::FormatError);
    }
    CHECK(ray::doc::commandFromJson(R"({"args":{},"label":"","name":"x","schema":2})").error().code ==
          ErrorCode::FormatTooNew);
    CHECK(ray::doc::commandLogFromJson("{}").error().code == ErrorCode::FormatError);
    CHECK(ray::doc::commandLogFromJson(R"([{"args":{},"label":"","name":"x","schema":1},3])").error().code ==
          ErrorCode::FormatError);
}
