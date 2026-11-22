// Operation templates through the application (task T-057): the cam.template.* actions on a part closest to benchmark
// part 1 (PLAN.md 12.3), built here with the document's features because BP1 exists in the repository only as a STEP
// file (tests/corpus/files/raycad-synthetic/fixture_plate_*.stp), which carries no thread: a 6.00 x 4.00 x 0.75 in
// 6061 plate with two rectangular pockets 0.375 deep (without their 0.25 corner radii), four 1/4-20 UNC tapped
// through holes (hole feature, drilled #7) and four 0.266 in through holes with 0.44 x 0.26 in counterbores; the
// edge chamfer and the second setup are left out. On it: the "Tapped holes" set makes spot, #7 drill and tap in one
// action and one undo step; a missing tool refuses naming what is missing; automation needs the user's confirmation;
// saving an operation as a template, applying it, editing it (no hidden link), re-applying it, conflicts, rename and
// remove.
#include "engine_test_util.h"
#include "parametric_test_util.h"

#include <ray/api.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <map>
#include <string>

using Catch::Matchers::ContainsSubstring;
using namespace ray;
using ray::api::Caller;
namespace dt = ray::doc::test;

namespace {

constexpr std::chrono::milliseconds kWait = std::chrono::minutes(5);
constexpr double kIn = 25.4;
const std::string kBuiltin = std::string(RAY_SOURCE_DIR) + "/resources/library";

class TempDir {
public:
    TempDir()
    {
        path_ = std::filesystem::temp_directory_path() / "raycad-template-tests" / Uuid::generate().toString();
        std::filesystem::create_directories(path_);
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    std::filesystem::path path() const { return path_; }
    std::string file(const std::string& name) const
    {
        const std::u8string s = (path_ / name).u8string();
        return std::string(reinterpret_cast<const char*>(s.data()), s.size());
    }

private:
    std::filesystem::path path_;
};

std::unique_ptr<api::HeadlessApp> makeApp(const std::string& userDataDir, const std::string& builtin = kBuiltin)
{
    api::AppOptions options;
    options.workerThreads = 4;
    options.userDataDir = userDataDir;
    options.builtinLibraryDir = builtin;
    auto app = api::makeHeadlessApp(std::move(options));
    REQUIRE(app != nullptr);
    return app;
}

Result<api::ActionResult> tryInvoke(api::HeadlessApp& app, const std::string& action, const std::string& args,
                                    Caller caller = Caller::User)
{
    return app.wait(app.actions().invoke(action, args, caller), kWait);
}

std::string invoke(api::HeadlessApp& app, const std::string& action, const std::string& args)
{
    auto done = tryInvoke(app, action, args);
    if (!done) {
        FAIL(action << " failed: " << done.error().message);
    }
    return done->resultJson;
}

void run(api::HeadlessApp& app, const doc::Command& c)
{
    invoke(app, c.name, c.argsJson);
}

// Numbers in mm with enough digits.
std::string mm(double v)
{
    return dt::num(v);
}

std::string onTop()
{
    return R"({"datum":")" + dt::uuid('f', 4) + R"("})";
}

// The part (body b), built through the feature actions.
void buildPlate(api::HeadlessApp& app)
{
    const double L = 6 * kIn, W = 4 * kIn, T = 0.75 * kIn;
    run(app, dt::addFeature("sketch", 2, dt::sketchProps(R"({"principal":"xy"})", dt::SketchJson().rect(1, 0, 0, L, W))));
    run(app, dt::addFeature("extrude", 3, dt::extrudeProps(2, mm(T), "new", 'b')));
    run(app, dt::addFeature("datum_plane", 4, R"({"base":{"principal":"xy"},"offset":)" + mm(T) + "}"));
    // Two closed pockets 0.375 in deep, where the corpus generator puts them.
    const double pd = 0.375 * kIn;
    run(app, dt::addFeature("sketch", 5,
                            dt::sketchProps(onTop(), dt::SketchJson().rect(1, L * 0.3 - L * 0.125, W * 0.5 - W * 0.225,
                                                                          L * 0.3 + L * 0.125, W * 0.5 + W * 0.225))));
    run(app, dt::addFeature("extrude", 6, dt::extrudeProps(5, mm(pd), "cut", 'b', R"(,"direction":"reverse")")));
    run(app, dt::addFeature("sketch", 7,
                            dt::sketchProps(onTop(), dt::SketchJson().rect(1, L * 0.7 - L * 0.1, W * 0.5 - W * 0.175,
                                                                          L * 0.7 + L * 0.1, W * 0.5 + W * 0.175))));
    run(app, dt::addFeature("extrude", 8, dt::extrudeProps(7, mm(pd), "cut", 'b', R"(,"direction":"reverse")")));
    // Four 1/4-20 UNC tapped through holes, drilled #7 (0.201 in) as the thread table gives it.
    run(app, dt::addFeature("sketch", 9,
                            dt::sketchProps(onTop(), dt::SketchJson()
                                                         .point(1, L * 0.46, W * 0.1)
                                                         .point(2, L * 0.54, W * 0.1)
                                                         .point(3, L * 0.46, W * 0.9)
                                                         .point(4, L * 0.54, W * 0.9))));
    run(app, dt::addFeature("hole", 10,
                            R"({"sketch":")" + dt::uuid('f', 9) + R"(","target":")" + dt::uuid('b', 1) +
                                R"(","kind":"tapped","end":"through","thread":{"designation":"1/4-20 UNC",)"
                                R"("standard":"unc","nominalDiameter":6.35,"tpi":20,"tapDrill":5.1054}})"));
    // Four 0.266 in through holes with 0.44 x 0.26 in counterbores.
    run(app, dt::addFeature("sketch", 11,
                            dt::sketchProps(onTop(), dt::SketchJson()
                                                         .point(1, L * 0.08, W * 0.12)
                                                         .point(2, L * 0.92, W * 0.12)
                                                         .point(3, L * 0.08, W * 0.88)
                                                         .point(4, L * 0.92, W * 0.88))));
    run(app, dt::addFeature("hole", 12,
                            R"({"sketch":")" + dt::uuid('f', 11) + R"(","target":")" + dt::uuid('b', 1) +
                                R"(","kind":"counterbore","diameter":)" + mm(0.266 * kIn) +
                                R"(,"end":"through","counterbore":{"diameter":)" + mm(0.44 * kIn) + R"(,"depth":)" +
                                mm(0.26 * kIn) + "}}"));
    api::OpenDocument* od = app.active();
    REQUIRE(od != nullptr);
    REQUIRE(app.waitForBuild(*od, kWait));
    for (const auto& r : od->document->build()->reports) {
        INFO(r.message);
        REQUIRE(r.status == doc::FeatureStatus::Ok);
    }
}

const std::string kSetup = ray::cam::test::id('a', 1);

// Setup 1 on the plate: the shipped 6061-T6 and the Haas machine, embedded from the library.
void addSetup(api::HeadlessApp& app)
{
    const lib::LibraryStack* stack = app.libraries();
    REQUIRE(stack != nullptr);
    std::string material, machine, machineId, materialId;
    for (const lib::Material& m : stack->materials()) {
        if (m.name == "6061-T6") {
            material = lib::toJson(m);
            materialId = m.id.value.toString();
        }
    }
    for (const lib::MachineDef& m : stack->machines()) {
        if (m.controlDialect == "haas-ngc") {
            machine = lib::toJson(m);
            machineId = m.id.value.toString();
        }
    }
    REQUIRE_FALSE(material.empty());
    REQUIRE_FALSE(machine.empty());
    invoke(app, "cam.setup.add",
           R"({"id":")" + kSetup + R"(","name":"Op 1 - top","partBodies":[")" + dt::uuid('b', 1) +
               R"("],"machine":")" + machineId + R"(","material":")" + materialId + R"(","library":{"machines":[)" +
               machine + R"(],"materials":[)" + material + "]}}");
}

// A small reader of the action results.
struct Json {
    std::string text;
    // The string value of the first "key":"..." after `from`.
    std::string str(const std::string& key, std::size_t from = 0) const
    {
        const std::string needle = "\"" + key + "\":\"";
        const auto at = text.find(needle, from);
        if (at == std::string::npos) {
            return {};
        }
        const auto start = at + needle.size();
        return text.substr(start, text.find('"', start) - start);
    }
    std::string number(const std::string& key, std::size_t from = 0) const
    {
        const std::string needle = "\"" + key + "\":";
        const auto at = text.find(needle, from);
        if (at == std::string::npos) {
            return {};
        }
        const auto start = at + needle.size();
        return text.substr(start, text.find_first_of(",}]", start) - start);
    }
};

const cam::Setup& setupOf(const doc::DocState& state)
{
    REQUIRE(state.cam != nullptr);
    REQUIRE(state.cam->setups.size() == 1);
    return state.cam->setups.front();
}

std::vector<const cam::Operation*> operationsOf(const doc::DocState& state)
{
    std::vector<const cam::Operation*> out;
    for (const OperationId& id : setupOf(state).operations) {
        out.push_back(&state.cam->operations.at(id));
    }
    return out;
}

const lib::Tool& embeddedTool(const doc::DocState& state, const cam::Operation& op)
{
    return state.cam->tools.at(op.tool.tool).item;
}

std::shared_ptr<const cam::Toolpath> generate(api::HeadlessApp& app, const OperationId& op)
{
    api::OpenDocument* od = app.active();
    auto job = app.cam()->generate(od->document->state(), od->document->build(), op);
    auto path = app.wait(std::move(job), kWait);
    if (!path) {
        FAIL("generation failed: " << path.error().message);
    }
    return *path;
}

} // namespace

TEST_CASE("the tapped holes set on BP1 makes spot, #7 drill and tap with the right tools in one action and one undo "
          "step",
          "[cam][templates][api]")
{
    TempDir dir;
    auto app = makeApp(dir.file("user"));
    invoke(*app, "file.new", "{}");
    buildPlate(*app);
    addSetup(*app);
    api::OpenDocument* od = app->active();
    const Hash256 before = od->document->state()->stateHash();

    // The preview first: what would be made, and nothing changes.
    const Json preview{invoke(*app, "cam.template.preview", R"({"setup":")" + kSetup + R"(","set":"Tapped holes"})")};
    INFO(preview.text);
    CHECK(preview.number("holes") == "8");
    CHECK_THAT(preview.text, ContainsSubstring("\"label\":\"4 x 1/4-20 UNC tapped, through\""));
    CHECK_THAT(preview.text, ContainsSubstring("\"label\":\"4 x 6.756 mm, through, counterbored 11.176 x 6.604\""));
    CHECK(preview.number("ok") == "true");
    CHECK(preview.number("untouched") == "4"); // the counterbored holes are not tapped
    CHECK(od->document->state()->stateHash() == before);
    const auto spotAt = preview.text.find("\"name\":\"Spot drill\"");
    const auto drillAt = preview.text.find("\"name\":\"Tap drill, chip break\"");
    const auto tapAt = preview.text.find("\"name\":\"Tap\"");
    REQUIRE(spotAt != std::string::npos);
    REQUIRE(drillAt != std::string::npos);
    REQUIRE(tapAt != std::string::npos);
    CHECK(spotAt < drillAt);
    CHECK(drillAt < tapAt);

    // Apply: three ordinary operations, in order, each from its template.
    const Json applied{invoke(*app, "cam.template.apply", R"({"setup":")" + kSetup + R"(","set":"Tapped holes"})")};
    const doc::DocStatePtr state = od->document->state();
    const auto ops = operationsOf(*state);
    REQUIRE(ops.size() == 3);
    CHECK(ops[0]->name == "Spot drill");
    CHECK(ops[1]->name == "Tap drill, chip break");
    CHECK(ops[2]->name == "Tap");
    CHECK(embeddedTool(*state, *ops[0]).name == "1/4 in 90 deg carbide spot drill");
    CHECK(embeddedTool(*state, *ops[1]).name == "#7 (0.201 in) HSS-Co jobber drill");
    CHECK(embeddedTool(*state, *ops[2]).name == "1/4-20 UNC HSS-E spiral point tap");
    for (std::size_t i = 0; i < 3; ++i) {
        INFO(ops[i]->name);
        CHECK(ops[i]->type == "drill");
        CHECK(ops[i]->geometry.size() == 4);
        CHECK(ops[i]->tool.toolNumber == static_cast<int>(i + 1));
        const lib::OpTemplate* t = app->libraries()->opTemplate(Uuid::fromString(ops[i]->templateId));
        REQUIRE(t != nullptr);
        CHECK(t->name == ops[i]->name);
        CHECK(ops[i]->calculated.has_value());
        CHECK(ops[i]->heights.clearance == 15.24); // the operation panel's inch defaults
    }
    CHECK_THAT(ops[0]->paramsJson, ContainsSubstring("\"cycle\":\"spot\""));
    CHECK_THAT(ops[1]->paramsJson, ContainsSubstring("\"cycle\":\"chipBreak\""));
    CHECK_THAT(ops[2]->paramsJson, ContainsSubstring("\"cycle\":\"tap\""));
    CHECK(od->document->undoCommand()->name == "cam.batch");
    CHECK(od->document->undoCommand()->label == "Apply Template Set Tapped holes");
    CHECK(applied.str("id").size() == 36);

    // They generate: a spot, a chip-breaking cycle and a tap at the 1/4-20 pitch.
    const auto spot = generate(*app, ops[0]->id);
    const auto drill = generate(*app, ops[1]->id);
    const auto tap = generate(*app, ops[2]->id);
    CHECK(spot->moves.at(1).kind == cam::MoveKind::CycleDrill);
    CHECK(drill->moves.at(1).kind == cam::MoveKind::CycleChipBreak);
    CHECK(drill->moves.at(1).cyclePeck == 5'105'400); // 1 x the drill's diameter in aluminium
    CHECK(tap->moves.at(1).kind == cam::MoveKind::CycleTap);
    CHECK(tap->moves.at(1).threadPitchNm == 1'270'000);
    std::size_t cycles = 0;
    for (const cam::Move& m : tap->moves) {
        cycles += m.kind == cam::MoveKind::CycleTap ? 1 : 0;
    }
    CHECK(cycles == 4);

    // One undo step takes all three away; redo brings them back.
    invoke(*app, "edit.undo", "{}");
    CHECK(od->document->state()->stateHash() == before);
    invoke(*app, "edit.redo", "{}");
    CHECK(operationsOf(*od->document->state()).size() == 3);

    // The drilled-holes set on the counterbored holes only (a kind of hole): spot and the letter H drill.
    invoke(*app, "cam.template.apply",
           R"({"setup":")" + kSetup + R"(","set":"Drilled holes","holes":{"kind":"hole.counterbored"}})");
    const auto more = operationsOf(*od->document->state());
    REQUIRE(more.size() == 5);
    CHECK(more[3]->name == "Spot drill");
    CHECK(more[4]->name == "Drill");
    CHECK(embeddedTool(*od->document->state(), *more[4]).name == "H (0.266 in) HSS-Co jobber drill");
    CHECK(more[3]->tool.toolNumber == 4); // the 10 mm spot drill is a new tool
    CHECK(more[4]->tool.toolNumber == 5);
}

TEST_CASE("picked holes and hole filters choose the targets", "[cam][templates][api]")
{
    TempDir dir;
    auto app = makeApp(dir.file("user"));
    invoke(*app, "file.new", "{}");
    buildPlate(*app);
    addSetup(*app);
    api::OpenDocument* od = app->active();
    // One tapped hole picked by a face of it (its bore, from a reference as the viewport pick makes it).
    const auto build = od->document->build();
    const doc::BuiltBody& body = dt::bodyOf(*build, 'b');
    auto resolver = naming::makeResolver({naming::ResolverBody{dt::bodyId('b'), body.map, body.shape}});
    JobContext ctx;
    auto holes = cam::recognizeHoles(body.shape, Xform::identity(), ctx);
    REQUIRE(holes);
    const auto picked = std::find_if(holes->begin(), holes->end(), [](const cam::RecognizedHole& h) {
        return std::abs(toMm(h.x) - 6 * kIn * 0.46) < 1e-6 && std::abs(toMm(h.y) - 4 * kIn * 0.1) < 1e-6;
    });
    REQUIRE(picked != holes->end());
    const std::string ref = naming::toJson(resolver->makeRef(dt::bodyId('b'), picked->faces.front()));
    const Json one{invoke(*app, "cam.template.preview",
                          R"({"setup":")" + kSetup + R"(","set":"Tapped holes","geometry":[)" + ref + "]}")};
    CHECK(one.number("targets") == "1");
    CHECK(one.number("holes", one.text.find("\"operations\"")) == "1");
    const Json filtered{invoke(*app, "cam.template.preview",
                               R"({"setup":")" + kSetup +
                                   R"(","set":"Drilled holes","holes":{"diameter":6.7564,"through":true}})")};
    CHECK(filtered.number("targets") == "4");
    const Json none{invoke(*app, "cam.template.preview",
                           R"({"setup":")" + kSetup + R"(","set":"Tapped holes","holes":{"threadSpec":"M6x1"}})")};
    CHECK(none.number("ok") == "false");
    CHECK_THAT(none.str("message"), ContainsSubstring("no recognised hole"));
    auto refused = tryInvoke(*app, "cam.template.apply",
                             R"({"setup":")" + kSetup + R"(","set":"Tapped holes","holes":{"threadSpec":"M6x1"}})");
    REQUIRE_FALSE(refused);
    CHECK(operationsOf(*od->document->state()).empty());
}

TEST_CASE("a missing tool refuses the whole application, naming the tool and the holes", "[cam][templates][api]")
{
    TempDir dir;
    // The built-in library without its 1/4-20 tap.
    const std::filesystem::path library = dir.path() / "builtin";
    std::filesystem::copy(std::filesystem::path(kBuiltin), library, std::filesystem::copy_options::recursive);
    {
        auto opened = lib::ILibrary::open(kBuiltin);
        REQUIRE(opened);
        bool removed = false;
        for (const lib::Tool& t : (*opened)->tools()) {
            if (t.threadSpec == "1/4-20 UNC") {
                removed = std::filesystem::remove(library / "tools" / (t.id.value.toString() + ".tool.json"));
            }
        }
        REQUIRE(removed);
    }
    const std::u8string builtin = library.u8string();
    auto app = makeApp(dir.file("user"), std::string(reinterpret_cast<const char*>(builtin.data()), builtin.size()));
    invoke(*app, "file.new", "{}");
    buildPlate(*app);
    addSetup(*app);
    api::OpenDocument* od = app->active();
    const Hash256 before = od->document->state()->stateHash();
    auto refused = tryInvoke(*app, "cam.template.apply", R"({"setup":")" + kSetup + R"(","set":"Tapped holes"})");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::NotFound);
    INFO(refused.error().message);
    CHECK_THAT(refused.error().message,
               ContainsSubstring("Nothing was created") &&
                   ContainsSubstring("no tool in the libraries for 'Tap' (tap, thread 1/4-20 UNC) for 4 holes") &&
                   ContainsSubstring("1/4-20 UNC at X 70.104 Y 10.16"));
    CHECK(od->document->state()->stateHash() == before);
    // The preview says the same without failing.
    const Json preview{invoke(*app, "cam.template.preview", R"({"setup":")" + kSetup + R"(","set":"Tapped holes"})")};
    CHECK(preview.number("ok") == "false");
    CHECK(preview.str("rule") == "tap, thread 1/4-20 UNC");
}

TEST_CASE("applying templates is a CAM change: automation needs the user's confirmation", "[cam][templates][api]")
{
    TempDir dir;
    auto app = makeApp(dir.file("user"));
    std::map<std::string, api::ActionInfo> byId;
    for (const auto& info : app->actions().all()) {
        byId[info.id] = info;
    }
    for (const char* id : {"cam.template.apply", "cam.template.reapply", "cam.batch"}) {
        INFO(id);
        REQUIRE(byId.count(id) == 1);
        CHECK(byId[id].changesToolpathsOrGcode);
        CHECK(byId[id].changesDocument);
        CHECK(byId[id].category == "CAM");
    }
    for (const char* id : {"cam.template.preview", "cam.template.saveFrom", "cam.template.rename", "cam.template.remove"}) {
        INFO(id);
        REQUIRE(byId.count(id) == 1);
        CHECK_FALSE(byId[id].changesToolpathsOrGcode);
    }
    invoke(*app, "file.new", "{}");
    buildPlate(*app);
    addSetup(*app);
    api::OpenDocument* od = app->active();
    const Hash256 before = od->document->state()->stateHash();
    std::string asked;
    app->setConfirmHook([&](const api::ActionInfo& info, std::string_view, std::string_view) {
        asked = info.id;
        return false;
    });
    const std::string args = R"({"setup":")" + kSetup + R"(","set":"Tapped holes"})";
    auto declined = tryInvoke(*app, "cam.template.apply", args, Caller::Agent);
    REQUIRE_FALSE(declined);
    CHECK(declined.error().code == ErrorCode::Cancelled);
    CHECK(asked == "cam.template.apply");
    CHECK(od->document->state()->stateHash() == before);
    asked.clear();
    CHECK(tryInvoke(*app, "cam.template.preview", args, Caller::Agent)); // a preview changes nothing: not asked
    CHECK(asked.empty());
    app->setConfirmHook([](const api::ActionInfo&, std::string_view, std::string_view) { return true; });
    REQUIRE(tryInvoke(*app, "cam.template.apply", args, Caller::Script));
    CHECK(operationsOf(*od->document->state()).size() == 3);
}

TEST_CASE("an operation saved as a template applies elsewhere, has no hidden link, re-applies on request",
          "[cam][templates][api]")
{
    TempDir dir;
    auto app = makeApp(dir.file("user"));
    invoke(*app, "file.new", "{}");
    buildPlate(*app);
    addSetup(*app);
    api::OpenDocument* od = app->active();
    invoke(*app, "cam.template.apply", R"({"setup":")" + kSetup + R"(","set":"Tapped holes"})");
    const auto ops = operationsOf(*od->document->state());
    REQUIRE(ops.size() == 3);
    const std::string drillOp = ops[1]->id.value.toString();
    const std::string tapOp = ops[2]->id.value.toString();

    // The tap, with its tool rule and calculated feeds; into the user's library (the last root).
    const Json savedTap{invoke(*app, "cam.template.saveFrom",
                               R"({"operation":")" + tapOp + R"(","name":"Shop tap","set":"Shop","sequence":30})")};
    const Uuid tapTemplate = Uuid::fromString(savedTap.str("id"));
    lib::LibraryStack* stack = app->libraries();
    const lib::OpTemplate* t = stack->opTemplate(tapTemplate);
    REQUIRE(t != nullptr);
    CHECK(t->name == "Shop tap");
    CHECK(t->featureKind == "hole.tapped");
    CHECK(t->opType == "drill");
    CHECK_THAT(t->toolQueryJson, ContainsSubstring("\"threadSpec\":\"$feature.threadSpec\""));
    CHECK_THAT(t->extraJson, ContainsSubstring("\"mode\":\"calculated\"") && ContainsSubstring("\"set\":\"Shop\""));
    CHECK_THAT(t->paramsJson, ContainsSubstring("\"cycle\":\"tap\""));
    REQUIRE(t->material.has_value());
    CHECK(*stack->rootOf(lib::ItemKind::Template, tapTemplate) == stack->roots().size() - 1);
    CHECK(std::filesystem::exists(dir.path() / "user" / "library" / "templates" /
                                  (tapTemplate.toString() + ".optemplate.json")));

    // The drill, fixed feeds, its exact tool.
    const Json savedDrill{invoke(*app, "cam.template.saveFrom",
                                 R"({"operation":")" + drillOp +
                                     R"(","name":"Shop drill","feeds":"fixed","tool":"exact"})")};
    const Uuid drillTemplate = Uuid::fromString(savedDrill.str("id"));
    REQUIRE(stack->opTemplate(drillTemplate) != nullptr);
    const lib::OpTemplate shopDrill = *stack->opTemplate(drillTemplate); // a copy: a later save remerges the stack
    const lib::OpTemplate* d = &shopDrill;
    CHECK_THAT(d->toolQueryJson, ContainsSubstring("\"id\":\"" + ops[1]->tool.tool.value.toString() + "\""));
    CHECK_THAT(d->extraJson, ContainsSubstring("\"mode\":\"fixed\""));
    // And with the tool rule: a drill of the hole's diameter.
    const Json ruled{invoke(*app, "cam.template.saveFrom", R"({"operation":")" + drillOp + R"(","name":"Ruled"})")};
    const lib::OpTemplate* r = stack->opTemplate(Uuid::fromString(ruled.str("id")));
    REQUIRE(r != nullptr);
    CHECK_THAT(r->toolQueryJson, ContainsSubstring("\"prefer\":{\"diameter\":\"$feature.diameter\"}"));

    // Applied by id: an ordinary operation that names it.
    invoke(*app, "cam.template.apply",
           R"({"setup":")" + kSetup + R"(","templates":[")" + drillTemplate.toString() + R"("]})");
    auto now = operationsOf(*od->document->state());
    REQUIRE(now.size() == 4);
    const cam::Operation applied = *now[3];
    CHECK(applied.templateId == drillTemplate.toString());
    CHECK(applied.cutting.spindleRpm == ops[1]->cutting.spindleRpm);
    CHECK(applied.tool.toolNumber == ops[1]->tool.toolNumber); // the same tool keeps its number

    // The template changes in the library: the operation does not.
    {
        lib::OpTemplate edited = *d;
        edited.extraJson = R"({"feeds":{"mode":"fixed","rpm":1234,"feedCut":321},"sequence":0})";
        lib::ILibrary* user = stack->library(stack->roots().size() - 1);
        REQUIRE(user != nullptr);
        REQUIRE(user->save(edited));
        stack->remerge();
    }
    CHECK(od->document->state()->cam->operations.at(applied.id).cutting.spindleRpm == applied.cutting.spindleRpm);
    // Re-apply, explicitly: one undo step.
    invoke(*app, "cam.template.reapply", R"({"operation":")" + applied.id.value.toString() + R"("})");
    const cam::Operation& again = od->document->state()->cam->operations.at(applied.id);
    CHECK(again.cutting.spindleRpm == 1234);
    CHECK(again.cutting.feedCut == 321);
    CHECK(again.geometry.size() == applied.geometry.size());
    CHECK(again.name == applied.name);
    CHECK(od->document->undoCommand()->label == "Re-apply Template Shop drill");
    invoke(*app, "edit.undo", "{}");
    CHECK(od->document->state()->cam->operations.at(applied.id).cutting.spindleRpm == applied.cutting.spindleRpm);
    // An operation not made from a template has none to re-apply.
    invoke(*app, "cam.operation.edit", R"({"id":")" + drillOp + R"(","template":""})");
    auto notFrom = tryInvoke(*app, "cam.template.reapply", R"({"operation":")" + drillOp + R"("})");
    REQUIRE_FALSE(notFrom);
    CHECK_THAT(notFrom.error().message, ContainsSubstring("was not made from a template"));

    // Conflicts: saving over a template that changed since it was shown is refused (the tool library's check).
    auto stale = tryInvoke(*app, "cam.template.saveFrom",
                           R"({"operation":")" + tapOp + R"(","id":")" + tapTemplate.toString() + R"(","base":")" +
                               std::string(64, '0') + R"("})");
    REQUIRE_FALSE(stale);
    CHECK(stale.error().code == ErrorCode::Locked);
    CHECK_THAT(stale.error().message, ContainsSubstring("changed in the library by someone else"));
    // Rename and remove; a built-in template is read-only.
    invoke(*app, "cam.template.rename", R"({"id":")" + tapTemplate.toString() + R"(","name":"Shop tap, rigid"})");
    CHECK(stack->opTemplate(tapTemplate)->name == "Shop tap, rigid");
    const Uuid builtinId = Uuid::fromString(ops[0]->templateId);
    auto readOnly = tryInvoke(*app, "cam.template.rename", R"({"id":")" + builtinId.toString() + R"(","name":"X"})");
    REQUIRE_FALSE(readOnly);
    CHECK_THAT(readOnly.error().message, ContainsSubstring("read-only"));
    invoke(*app, "cam.template.remove", R"({"id":")" + tapTemplate.toString() + R"("})");
    CHECK(stack->opTemplate(tapTemplate) == nullptr);
    // A removed template cannot be re-applied, and says so.
    auto gone = tryInvoke(*app, "cam.template.apply",
                          R"({"setup":")" + kSetup + R"(","templates":[")" + tapTemplate.toString() + R"("]})");
    REQUIRE_FALSE(gone);
    CHECK(gone.error().code == ErrorCode::NotFound);
}

TEST_CASE("cam.batch applies several CAM commands as one and refuses as a whole", "[cam][templates][api]")
{
    TempDir dir;
    auto app = makeApp(dir.file("user"));
    invoke(*app, "file.new", "{}");
    buildPlate(*app);
    addSetup(*app);
    api::OpenDocument* od = app->active();
    const Hash256 before = od->document->state()->stateHash();
    // The second command names a setup that does not exist: nothing changes, and the message names it.
    auto refused = tryInvoke(
        *app, "cam.batch",
        R"({"commands":[{"name":"cam.setup.edit","args":{"id":")" + kSetup +
            R"(","name":"Renamed"}},{"name":"cam.setup.edit","args":{"id":")" + ray::cam::test::id('a', 9) +
            R"(","name":"Nope"}}]})");
    REQUIRE_FALSE(refused);
    CHECK_THAT(refused.error().message, ContainsSubstring("commands[1] (cam.setup.edit)"));
    CHECK(od->document->state()->stateHash() == before);
    invoke(*app, "cam.batch",
           R"({"commands":[{"name":"cam.setup.edit","args":{"id":")" + kSetup +
               R"(","name":"Renamed"}},{"name":"cam.setup.edit","args":{"id":")" + kSetup +
               R"(","programNumber":2002}}]})");
    CHECK(setupOf(*od->document->state()).name == "Renamed");
    CHECK(setupOf(*od->document->state()).programNumber == 2002);
    invoke(*app, "edit.undo", "{}");
    CHECK(od->document->state()->stateHash() == before);
    auto notCam = tryInvoke(*app, "cam.batch", R"({"commands":[{"name":"feature.remove","args":{}}]})");
    REQUIRE_FALSE(notCam);
}
