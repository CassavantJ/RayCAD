// Random command sequences: 1,000 commands then 1,000 undos return the initial state, redo returns the final one,
// random undo/redo interleavings match a model of the history, and a command log replays to the same state.
#include "commands_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

using ray::doc::Command;
using ray::doc::DocState;
using ray::doc::DocStatePtr;
using ray::doc::Parameter;
using ray::doc::test::cmd;
using ray::doc::test::DocFixture;
using ray::doc::test::hashOf;
using ray::doc::test::uuid;

namespace {

// Makes a random command that is valid for the given snapshot. Uses std::mt19937 (fully specified by the standard)
// and its raw output, not a distribution, so the sequence is the same with every standard library.
class RandomCommands {
public:
    explicit RandomCommands(std::uint32_t seed) : rng_(seed) {}

    Command next(const DocState& s)
    {
        for (;;) {
            switch (pick(12)) {
            case 0:
            case 1:
            case 2:
                return featureAdd(s);
            case 3:
            case 4:
                if (!s.features.empty()) {
                    return featureEdit(s);
                }
                break;
            case 5:
            case 6:
                if (!s.features.empty()) {
                    return cmd("feature.remove", R"({"id":")" + anyFeature(s) + R"("})", "Delete");
                }
                break;
            case 7:
                if (!s.features.empty()) {
                    return cmd("feature.reorder",
                               R"({"index":)" + std::to_string(pick(s.features.size())) + R"(,"id":")" + anyFeature(s) +
                                   R"("})",
                               "Reorder");
                }
                break;
            case 8:
                if (!s.features.empty()) {
                    return cmd("feature.suppress",
                               R"({"id":")" + anyFeature(s) + R"(","suppressed":)" + (pick(2) ? "true" : "false") + "}",
                               "Suppress");
                }
                break;
            case 9:
            case 10:
                return paramSet(s);
            default:
                return bodySet(s);
            }
        }
    }

private:
    std::size_t pick(std::size_t n) { return static_cast<std::size_t>(rng_() % n); }

    std::string anyFeature(const DocState& s) { return s.features[pick(s.features.size())].id.value.toString(); }

    std::string number()
    {
        // Spelled in several ways so canonicalisation is exercised: 12, 12.5, 1.25e1, -0.0625.
        const std::size_t n = pick(200);
        switch (pick(4)) {
        case 0:
            return std::to_string(n);
        case 1:
            return std::to_string(n) + ".5";
        case 2:
            return std::to_string(n) + ".25e1";
        default:
            return "-0.0625";
        }
    }

    std::string props()
    {
        // Random key order and whitespace; the stored props are canonical either way.
        std::string distance = R"("distance": ")" + std::to_string(pick(50)) + R"( mm")";
        std::string count = R"("count" : )" + number();
        std::string nested = R"("nested":{ "b":[1, )" + number() + R"(, true, null], "a":"éx" })";
        switch (pick(3)) {
        case 0:
            return "{" + distance + "," + count + "," + nested + "}";
        case 1:
            return "{ " + nested + " , " + distance + " , " + count + " }";
        default:
            return "{" + count + "}";
        }
    }

    Command featureAdd(const DocState& s)
    {
        static const char* const types[] = {"extrude", "fillet", "hole", "sketch", "import", "datum_plane"};
        const std::uint64_t n = ++features_;
        std::string args = R"({"id":")" + uuid('f', n) + R"(","type":")" + types[pick(6)] + R"(","name":"Feature )" +
                           std::to_string(n) + R"(","props":)" + props();
        if (pick(2)) {
            args += R"(,"index":)" + std::to_string(pick(s.features.size() + 1));
        }
        if (pick(4) == 0) {
            args += R"(,"suppressed":true)";
        }
        return cmd("feature.add", args + "}", "Add Feature " + std::to_string(n));
    }

    Command featureEdit(const DocState& s)
    {
        const std::string id = anyFeature(s);
        switch (pick(3)) {
        case 0:
            return cmd("feature.edit",
                       R"({"id":")" + id + R"(","name":"Edited )" + std::to_string(pick(1000)) + R"("})", "Rename");
        case 1:
            return cmd("feature.edit", R"({"props":)" + props() + R"(,"id":")" + id + R"("})", "Edit");
        default:
            return cmd("feature.edit", R"({"id":")" + id + R"(","name":"E","props":)" + props() + "}", "Edit");
        }
    }

    // Length parameters p<k>; p<k> refers only to parameters with a smaller k, so there is never a cycle, and
    // parameters are never renamed or removed here, so every reference stays valid.
    Command paramSet(const DocState& s)
    {
        std::vector<std::size_t> existing;
        for (std::size_t i = 0; i < s.parameters.size(); ++i) {
            existing.push_back(i);
        }
        if (!existing.empty() && pick(2)) {
            const Parameter& p = s.parameters[existing[pick(existing.size())]];
            return cmd("param.set",
                       R"({"id":")" + p.id.value.toString() + R"(","expression":")" + expression(s, p.name) + R"("})",
                       "Edit " + p.name);
        }
        const std::uint64_t k = ++params_;
        const std::string name = "p" + std::to_string(k);
        return cmd("param.set",
                   R"({"kind":"length","id":")" + uuid('a', k) + R"(","name":")" + name + R"(","expression":")" +
                       expression(s, name) + R"("})",
                   "Add " + name);
    }

    static std::uint64_t indexOfName(const std::string& name) { return std::stoull(name.substr(1)); }

    std::string expression(const DocState& s, const std::string& self)
    {
        std::vector<std::string> earlier;
        for (const Parameter& p : s.parameters) {
            if (indexOfName(p.name) < indexOfName(self)) {
                earlier.push_back(p.name);
            }
        }
        if (!earlier.empty() && pick(2)) {
            return earlier[pick(earlier.size())] + " * 2 + " + std::to_string(pick(10)) + " mm";
        }
        return std::to_string(pick(100)) + (pick(2) ? " mm" : " in");
    }

    Command bodySet(const DocState& s)
    {
        static const char* const roles[] = {"part", "fixture", "stock_model"};
        const std::string color = "[" + std::to_string(pick(256)) + "e-3," + std::to_string(pick(1000)) + "e-3,0.5,1]";
        if (!s.bodies.empty() && pick(2)) {
            const std::string id = s.bodies[pick(s.bodies.size())].id.value.toString();
            switch (pick(3)) {
            case 0:
                return cmd("body.set", R"({"id":")" + id + R"(","color":)" + color + "}", "Colour");
            case 1:
                return cmd("body.set", R"({"id":")" + id + R"(","visible":)" + (pick(2) ? "true" : "false") + "}",
                           "Show");
            default:
                return cmd("body.set", R"({"id":")" + id + R"(","role":")" + roles[pick(3)] + R"("})", "Role");
            }
        }
        const std::uint64_t n = ++bodies_;
        return cmd("body.set",
                   R"({"id":")" + uuid('b', n) + R"(","name":"Body )" + std::to_string(n) + R"(","role":")" +
                       roles[pick(3)] + R"(","color":)" + color + "}",
                   "Add body");
    }

    std::mt19937 rng_;
    std::uint64_t features_ = 0;
    std::uint64_t params_ = 0;
    std::uint64_t bodies_ = 0;
};

constexpr int kCommands = 1000;

} // namespace

TEST_CASE("1,000 random commands, 1,000 undos and 1,000 redos", "[doc][commands][undo]")
{
    DocFixture f;
    RandomCommands random(20261118);
    const DocStatePtr initial = f.doc->state();
    const std::string initialHash = hashOf(initial);

    std::vector<DocStatePtr> states{initial};
    std::vector<Command> log;
    for (int i = 0; i < kCommands; ++i) {
        const Command c = random.next(*f.doc->state());
        const DocStatePtr s = f.run(c);
        REQUIRE(s);
        states.push_back(s);
        log.push_back(c);
    }
    const DocStatePtr last = f.doc->state();
    const std::string finalHash = hashOf(last);
    CHECK(finalHash != initialHash);
    CHECK(f.doc->undoCount() == static_cast<std::size_t>(kCommands));
    // The run ends with a non-trivial document.
    CHECK(last->features.size() > 10);
    CHECK(last->parameters.size() > 10);
    CHECK_FALSE(last->bodies.empty());

    for (int i = kCommands; i > 0; --i) {
        REQUIRE(f.doc->undo());
        REQUIRE(f.doc->state() == states[static_cast<std::size_t>(i) - 1]);
    }
    CHECK_FALSE(f.doc->canUndo());
    CHECK(f.doc->redoCount() == static_cast<std::size_t>(kCommands));
    CHECK(hashOf(f.doc->state()) == initialHash);
    CHECK_FALSE(f.doc->dirty());

    for (int i = 1; i <= kCommands; ++i) {
        REQUIRE(f.doc->redo());
        REQUIRE(f.doc->state() == states[static_cast<std::size_t>(i)]);
    }
    CHECK_FALSE(f.doc->canRedo());
    CHECK(hashOf(f.doc->state()) == finalHash);

    // Every snapshot still has the hash it had when it was made: nothing was modified in place.
    for (std::size_t i = 0; i < states.size(); i += 97) {
        DocFixture replay;
        for (std::size_t j = 0; j < i; ++j) {
            REQUIRE(replay.run(log[j]));
        }
        CHECK(hashOf(replay.doc->state()) == hashOf(states[i]));
    }

    SECTION("the command log, written as JSON and replayed on a fresh document, gives the same state hash")
    {
        auto text = ray::doc::commandLogToJson(log);
        REQUIRE(text);
        CHECK(ray::doc::canonicalJson(*text) == *text);
        auto commands = ray::doc::commandLogFromJson(*text);
        REQUIRE(commands);
        REQUIRE(commands->size() == log.size());
        DocFixture replay;
        for (const Command& c : *commands) {
            REQUIRE(replay.run(c));
        }
        CHECK(hashOf(replay.doc->state()) == finalHash);

        // The commands the document recorded (undo entries) replay the same way.
        std::vector<Command> recorded;
        while (f.doc->canUndo()) {
            recorded.insert(recorded.begin(), *f.doc->undoCommand());
            REQUIRE(f.doc->undo());
        }
        auto recordedText = ray::doc::commandLogToJson(recorded);
        REQUIRE(recordedText);
        CHECK(*recordedText == *text);
    }

    SECTION("the same seed gives the same commands and the same final hash")
    {
        DocFixture again;
        RandomCommands same(20261118);
        for (int i = 0; i < kCommands; ++i) {
            REQUIRE(again.run(same.next(*again.doc->state())));
        }
        CHECK(hashOf(again.doc->state()) == finalHash);
    }
}

TEST_CASE("random interleavings of commands, undo and redo match a model of the history", "[doc][commands][undo]")
{
    DocFixture f;
    RandomCommands random(7);
    std::mt19937 rng(99);
    // Model: the snapshots on the undo path, and the redo stack.
    std::vector<DocStatePtr> done{f.doc->state()};
    std::vector<DocStatePtr> undone;
    int notifications = 0;
    f.doc->subscribe([&](DocStatePtr, ray::doc::BuildResultPtr) { ++notifications; });
    int changes = 0;
    for (int i = 0; i < 2000; ++i) {
        const auto r = rng() % 10;
        if (r < 2) {
            const bool ok = f.doc->undo().has_value();
            REQUIRE(ok == (done.size() > 1));
            if (ok) {
                undone.push_back(done.back());
                done.pop_back();
                ++changes;
            }
        } else if (r < 4) {
            const bool ok = f.doc->redo().has_value();
            REQUIRE(ok == !undone.empty());
            if (ok) {
                done.push_back(undone.back());
                undone.pop_back();
                ++changes;
            }
        } else {
            const DocStatePtr s = f.run(random.next(*f.doc->state()));
            REQUIRE(s);
            done.push_back(s);
            undone.clear();
            ++changes;
        }
        REQUIRE(f.doc->state() == done.back());
        REQUIRE(f.doc->undoCount() == done.size() - 1);
        REQUIRE(f.doc->redoCount() == undone.size());
        if (i % 100 == 0) {
            f.ui.runPending();
            REQUIRE(notifications == changes);
        }
    }
    f.ui.runPending();
    CHECK(notifications == changes);
}
