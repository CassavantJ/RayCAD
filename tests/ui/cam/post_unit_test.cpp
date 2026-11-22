// Posting without a window (task T-047): the G-code viewer's tokens, index, find and message places
// (src/ui/cam/post_logic.h); the post properties as a schema form (ray_api/cam_post.h onto SchemaFormModel); and the
// gate, through the cam.post action and the cam_post steps of a headless app on a real part: every blocking state
// (stale, never generated, failed, still generating, broken reference, no tool, no machine, a setup that does not
// resolve, nothing to post) refuses with the operation's name and reason and writes no file; the re-check before the
// file is put in place refuses an operation that went stale meanwhile; a script or an agent needs the user's
// confirmation; a good setup posts, and posting it again unchanged writes the same bytes.
#include "cam/operation_form.h"
#include "cam/post_logic.h"
#include "engine_test_util.h" // the CAM engine tests' test-only operation type and library JSON (tests/unit/cam)

#include <ray/api.h>
#include <ray_api/cam_post.h>
#include <ray_api/cam_setup.h>

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using namespace ray;
using namespace ray::ui::camui;
using Catch::Matchers::ContainsSubstring;
namespace cp = ray::api::cam_post;
namespace ct = ray::cam::test;
namespace fs = std::filesystem;

namespace {

constexpr auto kWait = std::chrono::minutes(3);
const std::string kPlate = RAY_CORPUS_DIR "/raycad-synthetic/plate_thru_holes_1.stp";
// The shipped "Generic VMC" (haas-ngc), unverified (resources/library/machines).
const std::string kVmc = "b903ede7-5123-4c26-bc15-466e92fba4c6";

std::string spans(std::string_view line)
{
    std::string out;
    for (const GSpan& s : tokenize(line)) {
        if (!out.empty())
            out += ' ';
        out += std::string(tokenName(s.kind)) + ":" + std::string(line.substr(s.start, s.length));
    }
    return out;
}

std::string utf8(const fs::path& p)
{
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

class TempDir {
public:
    TempDir()
    {
        std::random_device rd;
        path_ = fs::temp_directory_path() / ("raycad-t047-ui-" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(path_);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    std::string dir() const { return utf8(path_); }
    std::vector<std::string> files() const
    {
        std::vector<std::string> out;
        for (const auto& e : fs::directory_iterator(path_))
            out.push_back(utf8(e.path().filename()));
        std::sort(out.begin(), out.end());
        return out;
    }

private:
    fs::path path_;
};

std::string readAll(const std::string& path)
{
    std::ifstream in(fs::path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size())),
                     std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// ---------- a headless app with the plate, a setup on the shipped Generic VMC and test operations ----------
struct Shop {
    TempDir userData;
    std::unique_ptr<api::HeadlessApp> app;
    api::OpenDocument* od = nullptr;

    Shop()
    {
        ct::Gate::instance().reset();
        api::AppOptions options;
        options.workerThreads = 4;
        options.userDataDir = userData.dir();
        app = api::makeHeadlessApp(std::move(options));
        REQUIRE(app != nullptr);
        app->operationTypes()->add(std::make_unique<ct::TestOperation>());
        auto opened = app->wait(app->open(kPlate), kWait);
        REQUIRE(opened);
        od = *opened;
        REQUIRE(app->waitForBuild(*od, kWait));
    }
    ~Shop() { ct::Gate::instance().open = true; }

    std::string invoke(const std::string& action, const std::string& args, api::Caller caller = api::Caller::User)
    {
        auto r = app->wait(app->actions().invoke(action, args, caller), kWait);
        if (!r)
            FAIL(action << " failed: " << r.error().message);
        return r->resultJson;
    }
    Error fails(const std::string& action, const std::string& args, api::Caller caller = api::Caller::User)
    {
        auto r = app->wait(app->actions().invoke(action, args, caller), kWait);
        if (r)
            FAIL(action << " should have failed");
        return r.error();
    }
    std::string bodyId() const { return od->document->state()->bodies.at(0).id.value.toString(); }
    std::string machineJson() const
    {
        const lib::MachineDef* m = app->libraries()->machine(MachineId{Uuid::fromString(kVmc)});
        REQUIRE(m != nullptr);
        return api::cam_setup::machineJson(*m);
    }
    void addSetup(std::uint64_t n, const std::string& extra = {})
    {
        invoke("cam.setup.add", R"({"id":")" + ct::id('a', n) + R"(","name":"Setup )" + std::to_string(n) +
                                    R"(","partBodies":[")" + bodyId() + R"("],"machine":")" + kVmc +
                                    R"(","library":{"machines":[)" + machineJson() + "]}" + extra + "}");
    }
    void addOp(std::uint64_t n, std::uint64_t setup, const std::string& mode, const std::string& geometry = "[]",
               const std::string& extra = {})
    {
        const doc::Command c = ct::opAdd(n, setup, 1, geometry, R"({"mode":")" + mode + "\"}", extra);
        invoke(c.name, c.argsJson);
    }
    void generate(std::uint64_t op, bool wait = true)
    {
        auto job = app->cam()->generate(od->document->state(), od->document->build(), ct::opId(op));
        if (!wait)
            return;
        (void)app->wait(job, kWait);
        // The engine files the result on the UI thread, after the job.
        REQUIRE(app->runUntil([&] { return statuses().at(ct::opId(op)).status != cam::OpStatus::Generating; }, kWait));
    }
    std::map<OperationId, cam::OpState> statuses()
    {
        return app->cam()->status(od->document->state(), od->document->build());
    }
    cp::Inputs inputs(std::uint64_t setup)
    {
        cp::Inputs in;
        in.setup = SetupId{Uuid::fromString(ct::id('a', setup))};
        in.state = od->document->state();
        in.build = od->document->build();
        in.statuses = statuses();
        for (const auto& r : app->libraries()->roots())
            in.libraryRoots.push_back(r.dir);
        in.documentName = "plate";
        return in;
    }
    std::string postArgs(std::uint64_t setup, const std::string& dir, const std::string& extra = {}) const
    {
        std::string d;
        for (const char c : dir) {
            if (c == '\\' || c == '"')
                d.push_back('\\');
            d.push_back(c);
        }
        return R"({"setup":")" + ct::id('a', setup) + R"(","directory":")" + d + "\"" + extra + "}";
    }
    // A face reference that cannot resolve (its names are wrong), for a broken-reference operation.
    std::string brokenFace()
    {
        const std::string r = invoke("model.reference", R"({"body":")" + bodyId() + R"(","kind":"face","index":0})");
        QJsonObject ref = QJsonDocument::fromJson(QByteArray::fromStdString(r)).object().value("ref").toObject();
        QJsonArray names;
        for (int i = 0; i < ref.value("names").toArray().size(); ++i)
            names.append(QStringLiteral("0102030405060708091011121314151617181920212223242526272829303132"));
        ref.insert("names", names);
        ref.remove("neighbourNames");
        ref.remove("hint");
        return QJsonDocument(QJsonArray{ref}).toJson(QJsonDocument::Compact).toStdString();
    }
};

} // namespace

// ======================================================================================================== tokens
TEST_CASE("G-code tokens: every kind of word the posts write", "[cam][post][ui]")
{
    CHECK(spans("N10 G0 G90 G54 X12.5 Y-3. Z.25") ==
          "sequence:N10 text:  g:G0 text:  g:G90 text:  g:G54 text:  axis:X12.5 text:  axis:Y-3. text:  axis:Z.25");
    CHECK(spans("G2 X1. Y2. I-0.5 J0 K1 R5. F250.") ==
          "g:G2 text:  axis:X1. text:  axis:Y2. text:  axis:I-0.5 text:  axis:J0 text:  axis:K1 text:  axis:R5. "
          "text:  feed:F250.");
    CHECK(spans("S8000 M3") == "feed:S8000 text:  m:M3");
    CHECK(spans("T1 M6 H1 D1") == "word:T1 text:  m:M6 text:  word:H1 text:  word:D1");
    CHECK(spans("G83 Z-10. R2. Q1.5 P0.300") ==
          "g:G83 text:  axis:Z-10. text:  axis:R2. text:  word:Q1.5 text:  word:P0.300");
    CHECK(spans("O01001 (BRACKET REV A)") == "program:O01001 text:  comment:(BRACKET REV A)");
    CHECK(spans("(t1 d6. flat end mill)") == "comment:(t1 d6. flat end mill)"); // Haas comments are lower case
    CHECK(spans("G1 X1. ; to the end") == "g:G1 text:  axis:X1. text:  comment:; to the end");
    CHECK(spans("%") == "text:%");
    CHECK(spans("") == "");
    CHECK(spans("(unclosed") == "comment:(unclosed");
    CHECK(spans("G1(c)X1.") == "g:G1 comment:(c) axis:X1.");
    // A letter with no number is text; lower-case words still read as words.
    CHECK(spans("X") == "text:X");
    CHECK(spans("g1 x2.") == "g:g1 text:  axis:x2.");
    CHECK(spans("E0.25 U1 V2 W3 A90. B0 C45.") ==
          "word:E0.25 text:  axis:U1 text:  axis:V2 text:  axis:W3 text:  axis:A90. text:  axis:B0 text:  axis:C45.");
}

TEST_CASE("G-code index: lines, both directions of the line map, find", "[cam][post][ui]")
{
    for (const char* eol : {"\r\n", "\n"}) {
        const std::string e = eol;
        auto text = std::make_shared<const std::string>("%" + e + "O1001" + e + "G0 X1." + e + "G1 X2. F100." + e +
                                                        "G1 Y2." + e + "M30" + e + "%" + e);
        std::vector<post::GcodeLineMap> map{{2, 0, 0}, {3, 0, 1}, {4, 0, 1}, {4, 1, 0}};
        auto index = buildIndex(text, map);
        REQUIRE(index->lineCount() == 7);
        CHECK(index->line(0) == "%");
        CHECK(index->line(3) == "G1 X2. F100.");
        CHECK(index->line(6) == "%");
        CHECK(index->line(7).empty());
        CHECK(index->lines(2, 3) == "G0 X1." + e + "G1 X2. F100." + e);
        CHECK_FALSE(index->moveOf(0));
        CHECK_FALSE(index->moveOf(5));
        REQUIRE(index->moveOf(3));
        CHECK(index->moveOf(3)->move == 1);
        CHECK(index->moveOf(4)->section == 0); // the first entry of a line
        const auto lines = index->linesOf(MoveRef{0, 1});
        REQUIRE(lines);
        CHECK(lines->first == 3);
        CHECK(lines->second == 4);
        CHECK(index->linesOf(MoveRef{1, 0})->first == 4);
        CHECK_FALSE(index->linesOf(MoveRef{0, 9}));
        const auto hits = findAll(*index, "x");
        REQUIRE(hits.size() == 2);
        CHECK(hits[0].line == 2);
        CHECK(hits[0].column == 3);
        CHECK(findAll(*index, "g1 ").size() == 2);
        CHECK(findAll(*index, "").empty());
        CHECK(findAll(*index, "nothing").empty());
    }
    auto empty = buildIndex(std::make_shared<const std::string>(), {});
    CHECK(empty->lineCount() == 0);
}

TEST_CASE("post messages are placed: the operation a section names, the script line of an error", "[cam][post][ui]")
{
    const std::string refusal = "haas-ngc.post.js:215: program name O01001 is longer than the machine's limit";
    auto p = attachTo(refusal);
    CHECK(p.scriptLine == 215);
    CHECK(p.script == "haas-ngc.post.js");
    CHECK(refusal.substr(p.textStart).rfind("program name", 0) == 0);
    CHECK_FALSE(p.section);
    p = attachTo("haas-ngc.post.js:255: section 2 (Drill 1): 9000 rpm is above the machine's 8100");
    CHECK(p.scriptLine == 255);
    CHECK(p.section == 2u);
    p = attachTo("a rapid move with cutter compensation on in section 0 (Contour)");
    CHECK(p.section == 0u);
    CHECK_FALSE(p.scriptLine);
    p = attachTo("includeDate is on but the post job carries no date");
    CHECK_FALSE(p.section);
    CHECK_FALSE(p.scriptLine);
}

// ======================================================================================================== properties
TEST_CASE("post properties become a schema form; the machine's values are the defaults", "[cam][post][ui]")
{
    const std::string script = std::string(RAY_SHIPPED_POSTS_DIR) + "/haas-ngc.post.js";
    auto info = cp::inspectPost(script);
    REQUIRE(info);
    lib::MachineDef machine{};
    machine.postPropertyOverrides["smoothing"] = "\"medium\"";
    machine.postPropertyOverrides["toolList"] = "false";
    machine.postPropertyOverrides["sequenceStart"] = "100";
    ray::ui::SchemaFormModel form;
    form.setSchemaJson(QString::fromStdString(cp::propertiesSchemaJson(*info, &machine)));
    CHECK(form.problems().isEmpty());
    CHECK(form.fieldCount() == static_cast<int>(info->properties.size()));
    // Every declared property, in declaration order, of its kind.
    const auto& fields = form.schema().fields;
    for (std::size_t i = 0; i < info->properties.size(); ++i) {
        const post::PostProperty& p = info->properties[i];
        INFO(p.key);
        CHECK(fields[i].key == QString::fromStdString(p.key));
        CHECK(fields[i].title == QString::fromStdString(p.title));
        using K = ray::ui::camui::FormField::Kind;
        switch (p.type) {
        case post::PostProperty::Type::Bool:
            CHECK(fields[i].kind == K::Boolean);
            break;
        case post::PostProperty::Type::Int:
            CHECK(fields[i].kind == K::Integer);
            break;
        case post::PostProperty::Type::Number:
            CHECK(fields[i].kind == K::Number);
            break;
        case post::PostProperty::Type::Enum:
            CHECK(fields[i].kind == K::Enum);
            CHECK(fields[i].enumValues.size() == static_cast<int>(p.enumValues.size()));
            break;
        case post::PostProperty::Type::String:
            CHECK(fields[i].kind == K::String);
            break;
        }
    }
    // The machine's values are the defaults: nothing to override while they are kept.
    CHECK(form.valueOf("smoothing").toString() == "medium");
    CHECK(form.valueOf("toolList").toBool() == false);
    CHECK(form.valueOf("sequenceStart").toInt() == 100);
    CHECK(form.valueOf("decimalStyle").toString() == "trim"); // the post's own default
    const auto json = [&] {
        return QJsonDocument(form.params()).toJson(QJsonDocument::Compact).toStdString();
    };
    CHECK(cp::propertyOverrides(*info, &machine, json()).empty());
    // Changed in the form: exactly the changes, as the host reads them.
    form.setValue("smoothing", "finish");
    form.setValue("toolList", true);
    form.setValue("sequenceStart", "20");
    form.setValue("peckReductionJ", "0.5");
    const auto overrides = cp::propertyOverrides(*info, &machine, json());
    CHECK(overrides ==
          std::map<std::string, std::string>{
              {"peckReductionJ", "0.5"}, {"sequenceStart", "20"}, {"smoothing", "finish"}, {"toolList", "true"}});
    // And back into a fresh form: the same values (what is remembered per setup).
    ray::ui::SchemaFormModel again;
    again.setSchemaJson(QString::fromStdString(cp::propertiesSchemaJson(*info, &machine)));
    again.load(QJsonDocument::fromJson(QByteArray::fromStdString(cp::propertyValuesJson(*info, overrides))).object());
    CHECK(again.valueOf("smoothing").toString() == "finish");
    CHECK(again.valueOf("peckReductionJ").toDouble() == 0.5);
    CHECK(cp::propertyOverrides(*info, &machine,
                                QJsonDocument(again.params()).toJson(QJsonDocument::Compact).toStdString()) ==
          overrides);
    // Every shipped post's properties map without a problem.
    for (const char* name : {"generic-iso", "fanuc", "haas-classic", "haas-ngc"}) {
        auto i = cp::inspectPost(std::string(RAY_SHIPPED_POSTS_DIR) + "/" + name + ".post.js");
        REQUIRE(i);
        ray::ui::SchemaFormModel f;
        f.setSchemaJson(QString::fromStdString(cp::propertiesSchemaJson(*i, nullptr)));
        INFO(name);
        CHECK(f.problems().isEmpty());
        CHECK(f.fieldCount() == static_cast<int>(i->properties.size()));
        CHECK(f.valid());
        for (const auto& field : f.schema().fields)
            CHECK(field.kind != ray::ui::camui::FormField::Kind::Unsupported);
    }
}

// ======================================================================================================== the gate
TEST_CASE("the gate names every blocking state, for each operation", "[cam][post][gate]")
{
    // A hand-made document: setup 1 with operations 1..6, every status the engine has.
    auto cam = std::make_shared<cam::CamState>();
    cam::Setup setup{};
    setup.id = SetupId{Uuid::fromString(ct::id('a', 1))};
    setup.name = "Op 1";
    setup.machine = MachineId{Uuid::fromString(ct::id('9', 1))};
    cam->machines[setup.machine] = cam::Embedded<lib::MachineDef>{};
    const ToolId tool{Uuid::fromString(ct::id('d', 1))};
    cam->tools[tool] = cam::Embedded<lib::Tool>{};
    std::map<OperationId, cam::OpState> statuses;
    const auto tp = std::make_shared<const cam::Toolpath>();
    const auto add = [&](std::uint64_t n, cam::OpStatus st, bool withPath, bool hasTool = true,
                         bool suppressed = false) {
        cam::Operation op{};
        op.id = ct::opId(n);
        op.name = "Op " + std::to_string(n);
        op.tool.tool = hasTool ? tool : ToolId{Uuid::fromString(ct::id('d', 9))};
        op.suppressed = suppressed;
        cam->operations[op.id] = op;
        setup.operations.push_back(op.id);
        cam::OpState s;
        s.status = st;
        s.toolpath = withPath ? tp : nullptr;
        s.message = st == cam::OpStatus::Failed ? "the generator said no" : "a face is gone";
        s.progress = 0.4;
        statuses[op.id] = s;
    };
    add(1, cam::OpStatus::UpToDate, true);
    add(2, cam::OpStatus::Stale, true);
    add(3, cam::OpStatus::Stale, false);
    add(4, cam::OpStatus::Generating, false);
    add(5, cam::OpStatus::Failed, false);
    add(6, cam::OpStatus::ReferenceBroken, false);
    add(7, cam::OpStatus::UpToDate, true, false);
    add(8, cam::OpStatus::Suppressed, false, true, true);
    cam->setups.push_back(setup);
    auto state = std::make_shared<doc::DocState>();
    state->cam = cam;

    const auto list = cp::blockers(*state, true, setup.id, statuses, Status{});
    std::vector<std::string> got;
    for (const auto& b : list)
        got.push_back(b.name + "=" + std::string(cp::reasonKey(b.reason)) + (b.generateFixes ? "+generate" : ""));
    CHECK(got == std::vector<std::string>{"Op 2=stale+generate", "Op 3=noToolpath+generate", "Op 4=generating",
                                          "Op 5=failed", "Op 6=referenceBroken", "Op 7=noTool"});
    CHECK_THAT(list[2].message, ContainsSubstring("40%"));
    CHECK_THAT(list[3].message, ContainsSubstring("the generator said no"));
    CHECK_THAT(list[4].message, ContainsSubstring("a face is gone"));
    CHECK_THAT(cp::blockersText(list), ContainsSubstring("Op 5: Generating its toolpath failed"));
    // The setup itself.
    CHECK(cp::reasonKey(cp::blockers(*state, false, setup.id, statuses, Status{}).at(0).reason) == "notBuilt");
    const Status broken = tl::unexpected(Error{ErrorCode::ReferenceBroken, "the Z face is gone", {}});
    const auto withBroken = cp::blockers(*state, true, setup.id, statuses, broken);
    CHECK(withBroken.front().reason == cp::BlockReason::SetupBroken);
    CHECK_THAT(withBroken.front().message, ContainsSubstring("the Z face is gone"));
    CHECK(cp::blockers(*state, true, SetupId{Uuid::fromString(ct::id('a', 7))}, statuses, Status{}).at(0).reason ==
          cp::BlockReason::NoSetup);
    // No machine; nothing to post.
    auto noMachine = std::make_shared<cam::CamState>(*cam);
    noMachine->machines.clear();
    for (const OperationId& id : setup.operations)
        noMachine->operations[id].suppressed = true;
    auto state2 = std::make_shared<doc::DocState>();
    state2->cam = noMachine;
    std::vector<cp::BlockReason> reasons;
    for (const auto& b : cp::blockers(*state2, true, setup.id, statuses, Status{}))
        reasons.push_back(b.reason);
    CHECK(reasons == std::vector<cp::BlockReason>{cp::BlockReason::NoMachine, cp::BlockReason::NoOperations});
    // Only good operations: nothing blocks.
    auto good = std::make_shared<cam::CamState>(*cam);
    for (std::uint64_t n = 2; n <= 7; ++n)
        good->operations[ct::opId(n)].suppressed = true;
    auto state3 = std::make_shared<doc::DocState>();
    state3->cam = good;
    CHECK(cp::blockers(*state3, true, setup.id, statuses, Status{}).empty());
}

TEST_CASE("cam.post refuses every blocking state and writes nothing; a good setup posts, the same bytes again",
          "[cam][post][gate][e2e]")
{
    Shop shop;
    TempDir out;
    shop.invoke("cam.library.embed", ct::library(ct::toolJson(1)));
    shop.addSetup(1);
    shop.addOp(1, 1, "plain");
    shop.generate(1);
    REQUIRE(shop.statuses().at(ct::opId(1)).status == cam::OpStatus::UpToDate);

    // Each blocking state alone next to the good operation: the action refuses, naming it; no file.
    struct Case {
        const char* what;
        std::function<void()> make;
        const char* expect;
    };
    shop.addOp(2, 1, "plain");
    // Inputs of its own (with operation 1's it would find operation 1's toolpath in the engine's cache, up to date).
    shop.invoke("cam.operation.edit", R"({"id":")" + ct::id('c', 2) + R"(","heights":{"bottom":-2}})");
    const auto only = [&](std::uint64_t n) {
        // Operation n is the only one besides operation 1 that is not suppressed.
        for (std::uint64_t k = 2; k <= 6; ++k)
            shop.invoke("cam.operation.suppress",
                        R"({"id":")" + ct::id('c', k) + R"(","suppressed":)" + (k == n ? "false" : "true") + "}");
    };
    shop.addOp(3, 1, "fail");
    shop.addOp(4, 1, "plain", shop.brokenFace());
    shop.addOp(5, 1, "block");
    shop.addOp(6, 1, "plain");
    shop.generate(6);
    // Operation 6 goes stale: its parameters change after it was generated.
    shop.invoke("cam.operation.edit", R"({"id":")" + ct::id('c', 6) + R"(","heights":{"bottom":-6}})");
    shop.generate(3);
    REQUIRE(shop.statuses().at(ct::opId(3)).status == cam::OpStatus::Failed);
    REQUIRE(shop.statuses().at(ct::opId(6)).status == cam::OpStatus::Stale);
    REQUIRE(shop.statuses().at(ct::opId(6)).toolpath != nullptr);

    const std::vector<std::pair<std::uint64_t, const char*>> cases{
        {2, "has not been generated"}, // never generated
        {3, "Generating its toolpath failed"},
        {4, "A geometry reference does not resolve"},
        {6, "out of date"}, // stale
    };
    for (const auto& [n, words] : cases) {
        INFO("operation " << n);
        only(n);
        const Error e = shop.fails("cam.post", shop.postArgs(1, out.dir()));
        CHECK(e.code == ErrorCode::PostFailed);
        CHECK_THAT(e.message, ContainsSubstring("Posting is blocked"));
        CHECK_THAT(e.message, ContainsSubstring("Op " + std::to_string(n) + ": "));
        CHECK_THAT(e.message, ContainsSubstring(words));
        // The steps refuse too, not only the action.
        JobContext ctx;
        auto run = cp::run(shop.inputs(1), cp::Settings{}, ctx);
        REQUIRE_FALSE(run);
        CHECK(run.error().code == ErrorCode::PostFailed);
        CHECK(out.files().empty());
    }
    // Still generating: operation 5 waits at the gate.
    only(5);
    ct::Gate::instance().open = false;
    shop.generate(5, false);
    REQUIRE(shop.app->runUntil([&] { return ct::Gate::instance().waiting.load() == 1; }, kWait));
    REQUIRE(shop.statuses().at(ct::opId(5)).status == cam::OpStatus::Generating);
    const Error generating = shop.fails("cam.post", shop.postArgs(1, out.dir()));
    CHECK_THAT(generating.message, ContainsSubstring("Op 5: Its toolpath is still being generated"));
    CHECK(out.files().empty());
    ct::Gate::instance().open = true;
    REQUIRE(
        shop.app->runUntil([&] { return shop.statuses().at(ct::opId(5)).status == cam::OpStatus::UpToDate; }, kWait));

    // Everything but the good operations suppressed: it posts, through the action.
    only(5);
    const std::string first = shop.invoke("cam.post", shop.postArgs(1, out.dir()));
    const QJsonObject r = QJsonDocument::fromJson(QByteArray::fromStdString(first)).object();
    const std::string path = r.value("path").toString().toStdString();
    const std::string name = r.value("fileName").toString().toStdString();
    CHECK_THAT(name, Catch::Matchers::EndsWith("_Setup 1.nc"));
    CHECK(out.files() == std::vector<std::string>{name});
    const std::string bytes = readAll(path);
    CHECK(bytes.size() == static_cast<std::size_t>(r.value("bytes").toInt()));
    CHECK_THAT(bytes, ContainsSubstring("O01001"));
    // Unchanged and posted again, replacing: the same bytes; refused by default; a numbered name on request.
    const Error exists = shop.fails("cam.post", shop.postArgs(1, out.dir()));
    CHECK_THAT(exists.message, ContainsSubstring("already exists"));
    const std::string second = shop.invoke("cam.post", shop.postArgs(1, out.dir(), R"(,"overwrite":"replace")"));
    CHECK(QJsonDocument::fromJson(QByteArray::fromStdString(second)).object().value("replaced").toBool());
    CHECK(readAll(path) == bytes);
    CHECK(QJsonDocument::fromJson(QByteArray::fromStdString(second)).object().value("hash") == r.value("hash"));
    shop.invoke("cam.post", shop.postArgs(1, out.dir(), R"(,"overwrite":"newName")"));
    const std::string numbered = name.substr(0, name.size() - 3) + "_2.nc";
    CHECK(out.files() == std::vector<std::string>{name, numbered});
    CHECK(readAll(out.dir() + "/" + numbered) == bytes);

    // A script or an agent needs the user's confirmation (changesToolpathsOrGcode): without a hook it is declined.
    const auto info = shop.app->actions().find("cam.post");
    REQUIRE(info);
    CHECK(info->changesToolpathsOrGcode);
    const Error agent =
        shop.fails("cam.post", shop.postArgs(1, out.dir(), R"(,"overwrite":"newName")"), api::Caller::Agent);
    CHECK(agent.code == ErrorCode::Cancelled);
    CHECK(out.files().size() == 2);
}

TEST_CASE("the re-check before writing refuses an operation that changed after it was posted", "[cam][post][gate]")
{
    Shop shop;
    TempDir out;
    shop.invoke("cam.library.embed", ct::library(ct::toolJson(1)));
    shop.addSetup(1);
    shop.addOp(1, 1, "plain");
    shop.generate(1);
    JobContext ctx;
    cp::Settings settings;
    settings.directory = out.dir();
    auto posted = cp::run(shop.inputs(1), settings, ctx);
    REQUIRE(posted);
    CHECK(posted->sectionOps == std::vector<OperationId>{ct::opId(1)});
    CHECK_FALSE(posted->machineVerified); // the shipped definitions are unverified
    const auto now = [&] {
        return shop.inputs(1);
    };
    REQUIRE(cp::stillCurrent(*posted, *now().state, true, now().statuses, Status{}));
    // The operation goes stale: refused, naming it; nothing written.
    shop.invoke("cam.operation.edit", R"({"id":")" + ct::id('c', 1) + R"(","heights":{"bottom":-7}})");
    auto stale = cp::stillCurrent(*posted, *now().state, true, now().statuses, Status{});
    REQUIRE_FALSE(stale);
    CHECK_THAT(stale.error().message, ContainsSubstring("changed while it was posted"));
    CHECK_THAT(stale.error().message, ContainsSubstring("Op 1"));
    // Generated again with other inputs: still refused (another toolpath than the one posted).
    shop.generate(1);
    auto other = cp::stillCurrent(*posted, *now().state, true, now().statuses, Status{});
    REQUIRE_FALSE(other);
    CHECK_THAT(other.error().message, ContainsSubstring("another toolpath"));
    // A tool number (not in the input hash, but in the program) changed: refused too.
    shop.invoke("cam.operation.edit", R"({"id":")" + ct::id('c', 1) + R"(","heights":{"bottom":-5}})");
    shop.generate(1);
    REQUIRE(cp::stillCurrent(*posted, *now().state, true, now().statuses, Status{}));
    shop.invoke("cam.operation.edit",
                R"({"id":")" + ct::id('c', 1) + R"(","tool":{"tool":")" + ct::id('d', 1) + R"(","number":7}})");
    shop.generate(1);
    auto renumbered = cp::stillCurrent(*posted, *now().state, true, now().statuses, Status{});
    REQUIRE_FALSE(renumbered);
    CHECK_THAT(renumbered.error().message, ContainsSubstring("tool or offsets"));
    CHECK(out.files().empty());
}
