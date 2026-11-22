// Operation templates (task T-057), the core (src/cam/templates.h): variables of hand-made holes and tools, expressions
// through the document's evaluator with its units, the template's rules, feature kinds and hole groups, the plan of an
// application against the shipped library (tool choice, peck alternatives, missing tools, fixed and calculated feeds,
// errors naming template, field and expression), and the shipped starter templates themselves.
#include "cam/json.h"
#include "cam/templates.h"

#include <ray/document.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using namespace ray;
namespace tpl = ray::cam::templates;

namespace {

const std::string kLibrary = std::string(RAY_SOURCE_DIR) + "/resources/library";

std::string readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

// The document's evaluator (doc::evaluate) as the API wires it: lengths mm, angles in degrees, over `params`.
tpl::EvaluateFn docEvaluator(std::map<std::string, double> params = {})
{
    return [params](std::string_view expression, tpl::FieldKind kind) -> Result<double> {
        const doc::Parameter::Kind k = kind == tpl::FieldKind::Length  ? doc::Parameter::Kind::Length
                                       : kind == tpl::FieldKind::Angle ? doc::Parameter::Kind::Angle
                                       : kind == tpl::FieldKind::Count ? doc::Parameter::Kind::Count
                                                                       : doc::Parameter::Kind::Unitless;
        auto v = doc::evaluate(expression, params, k);
        if (v && kind == tpl::FieldKind::Angle) {
            return *v * 57.29577951308232;
        }
        return v;
    };
}

cam::HoleSegment segment(double d, double zTop, double zBottom, bool conical = false, double angle = 0,
                         double bottomDiameter = -1)
{
    cam::HoleSegment s{};
    s.diameter = d;
    s.depth = zTop - zBottom;
    s.conical = conical;
    s.coneAngleDeg = angle;
    s.bottomDiameter = bottomDiameter < 0 ? d : bottomDiameter;
    s.zTop = toNm(zTop);
    s.zBottom = toNm(zBottom);
    return s;
}

// A through hole of diameter d in a plate `t` thick (top at Z 0), at (x, y).
cam::RecognizedHole throughHole(double d, double t, double x = 10, double y = 10)
{
    cam::RecognizedHole h{};
    h.segments = {segment(d, 0, -t)};
    h.bore = 0;
    h.through = true;
    h.bottom = cam::HoleBottom::Through;
    h.x = toNm(x);
    h.y = toNm(y);
    h.zTop = 0;
    h.zBottom = toNm(-t);
    return h;
}

cam::RecognizedHole tapped(const std::string& spec, double pitch, double major, double drill, double t, double x = 10,
                           double y = 10)
{
    cam::RecognizedHole h = throughHole(drill, t, x, y);
    h.threadSpec = spec;
    h.threadPitch = pitch;
    h.threadNominalDiameter = major;
    return h;
}

lib::OpTemplate makeTemplate(const std::string& name, const std::string& kind, const std::string& query,
                             const std::string& params, const std::string& extra = "")
{
    lib::OpTemplate t;
    t.id = Uuid::generate();
    t.name = name;
    t.opType = "drill";
    t.featureKind = kind;
    t.toolQueryJson = query;
    t.paramsJson = params;
    t.extraJson = extra;
    return t;
}

struct Fixture {
    lib::LibraryStack stack;
    std::shared_ptr<cam::OperationRegistry> types = std::make_shared<cam::OperationRegistry>();
    std::optional<lib::Material> aluminium;
    tpl::PlanContext ctx;
    Fixture()
    {
        stack.setRoots({kLibrary}, 1);
        cam::registerBuiltinOperationTypes(*types);
        for (const lib::Material& m : stack.materials()) {
            if (m.name == "6061-T6") {
                aluminium = m;
            }
        }
        REQUIRE(aluminium);
        ctx.queryTools = [this](std::string_view q) {
            return stack.queryTools(q, {});
        };
        ctx.evaluate = docEvaluator();
        ctx.types = types.get();
        ctx.material = &*aluminium;
        ctx.defaultHeights = cam::Heights{15, 5, 2, 0, 0};
    }
    std::vector<lib::OpTemplate> set(const std::string& name)
    {
        std::vector<lib::OpTemplate> out;
        for (const lib::OpTemplate& t : stack.templatesFor("", aluminium->id, MachineId{})) {
            auto r = tpl::rulesOf(t);
            REQUIRE(r);
            if (r->set == name) {
                out.push_back(t);
            }
        }
        return out;
    }
};

std::string paramText(const cam::Operation& op, const std::string& key)
{
    const std::string needle = "\"" + key + "\":";
    const auto at = op.paramsJson.find(needle);
    if (at == std::string::npos) {
        return {};
    }
    const auto end = op.paramsJson.find_first_of(",}", at);
    return op.paramsJson.substr(at + needle.size(), end - at - needle.size());
}

} // namespace

TEST_CASE("feature variables come from the recognised hole, with their units", "[cam][templates]")
{
    // A 6.6 mm through hole 20 deep under an 11 x 6.5 counterbore, and an M6x1 tapped blind hole with a 90 degree
    // countersink to 7 mm and a drill point.
    cam::RecognizedHole cb{};
    cb.segments = {segment(11, 0, -6.5), segment(6.6, -6.5, -20)};
    cb.bore = 1;
    cb.counterbored = true;
    cb.through = true;
    cb.zBottom = toNm(-20);
    const tpl::Vars v = tpl::featureVars(cb);
    CHECK(v.at("feature.diameter").number == 6.6);
    CHECK(v.at("feature.diameter").unit == tpl::VarUnit::Length);
    CHECK(v.at("feature.depth").number == Approx(20));
    CHECK(v.at("feature.depthRatio").number == Approx(20 / 6.6));
    CHECK(v.at("feature.depthRatio").unit == tpl::VarUnit::Plain);
    CHECK(v.at("feature.through").number == 1);
    CHECK(v.at("feature.blind").number == 0);
    CHECK(v.at("feature.counterbored").number == 1);
    CHECK(v.at("feature.counterboreDiameter").number == 11);
    CHECK(v.at("feature.counterboreDepth").number == Approx(6.5));
    CHECK(v.at("feature.topDiameter").number == 11);
    CHECK(v.at("feature.spotDiameter").number == 6.6); // a counterbore is not spotted: the bore is
    CHECK(v.count("feature.threadSpec") == 0);         // not tapped: no thread variables
    CHECK(v.count("feature.countersinkDiameter") == 0);

    cam::RecognizedHole m6{};
    m6.segments = {segment(7, 0, -1, true, 90, 5), segment(5, -1, -12), segment(5, -12, -13.5, true, 118, 0)};
    m6.bore = 1;
    m6.countersunk = true;
    m6.bottom = cam::HoleBottom::Conical;
    m6.zBottom = toNm(-13.5);
    m6.threadSpec = "M6x1";
    m6.threadPitch = 1;
    m6.threadNominalDiameter = 6;
    m6.threadDepth = 9;
    const tpl::Vars w = tpl::featureVars(m6);
    CHECK(w.at("feature.depth").number == Approx(12));
    CHECK(w.at("feature.totalDepth").number == Approx(13.5));
    CHECK(w.at("feature.blind").number == 1);
    CHECK(w.at("feature.tapped").number == 1);
    CHECK(w.at("feature.threadSpec").text == "M6x1");
    CHECK(w.at("feature.threadSpec").unit == tpl::VarUnit::Text);
    CHECK(w.at("feature.threadPitch").number == 1);
    CHECK(w.at("feature.threadsPerInch").number == Approx(25.4));
    CHECK(w.at("feature.threadMajor").number == 6);
    CHECK(w.at("feature.threadDepth").number == 9);
    CHECK(w.at("feature.countersunk").number == 1);
    CHECK(w.at("feature.countersinkDiameter").number == 7);
    CHECK(w.at("feature.countersinkAngle").number == 90);
    CHECK(w.at("feature.countersinkAngle").unit == tpl::VarUnit::Angle);
    CHECK(w.at("feature.spotDiameter").number == 7);

    // Every variable a hole can have is in the table (the documentation's list), and the "always" ones always are.
    std::set<std::string> listed;
    for (const tpl::VarInfo& info : tpl::featureVariables()) {
        listed.insert(std::string(info.name));
        if (info.always) {
            CHECK(v.count(std::string(info.name)) == 1);
            CHECK(w.count(std::string(info.name)) == 1);
        }
    }
    for (const auto& [name, var] : w) {
        CHECK(listed.count(name) == 1);
    }
}

TEST_CASE("tool variables include the point length", "[cam][templates]")
{
    lib::Tool drill;
    drill.type = lib::ToolType::Drill;
    drill.diameter = 6;
    drill.tipAngleDeg = 90;
    drill.flutes = 2;
    const tpl::Vars v = tpl::toolVars(drill);
    CHECK(v.at("tool.diameter").number == 6);
    CHECK(v.at("tool.pointAngle").number == 90);
    CHECK(v.at("tool.pointLength").number == Approx(3.0).epsilon(1e-12));
    CHECK(v.at("tool.type").text == "drill");
    CHECK(v.at("tool.flutes").number == 2);
    std::set<std::string> listed;
    for (const tpl::VarInfo& info : tpl::toolVariables()) {
        listed.insert(std::string(info.name));
    }
    for (const auto& [name, var] : v) {
        CHECK(listed.count(name) == 1);
    }
    CHECK(listed.size() == v.size());
}

TEST_CASE("the documentation lists every template variable", "[cam][templates]")
{
    const std::string doc = readFile(std::string(RAY_SOURCE_DIR) + "/docs/template_expressions.md");
    REQUIRE_FALSE(doc.empty());
    for (const auto& list : {tpl::featureVariables(), tpl::toolVariables()}) {
        for (const tpl::VarInfo& info : list) {
            INFO(info.name);
            CHECK(doc.find("`" + std::string(info.name) + "`") != std::string::npos);
        }
    }
}

TEST_CASE("expressions evaluate with the document's evaluator and units", "[cam][templates]")
{
    const lib::OpTemplate t = makeTemplate("Spot it", "hole", "{}", "{}");
    tpl::Vars vars = tpl::featureVars(tapped("1/4-20 UNC", 1.27, 6.35, 5.1054, 19.05));
    for (const auto& [name, v] : tpl::toolVars(lib::Tool{})) {
        vars.insert_or_assign(name, v);
    }
    vars["tool.diameter"].number = 5.1054;
    const tpl::EvaluateFn eval = docEvaluator({{"chamferExtra", 0.25}});

    SECTION("lengths, units, inches, functions and the document's parameters")
    {
        CHECK(*tpl::substitute("$feature.diameter + 0.2 mm", vars) == "(5.1054 mm) + 0.2 mm");
        CHECK(*tpl::evaluateField(t, "params.x", "$feature.threadMajor + 0.02 in", vars, tpl::FieldKind::Length,
                                  eval) == Approx(6.35 + 0.508));
        CHECK(*tpl::evaluateField(t, "params.x", "feature.depth / 4", vars, tpl::FieldKind::Length, eval) ==
              Approx(19.05 / 4));
        CHECK(*tpl::evaluateField(t, "params.x", "max(0.15 mm, min(0.3 mm, 0.03 * $feature.diameter))", vars,
                                  tpl::FieldKind::Length, eval) == Approx(0.153162).epsilon(1e-9));
        CHECK(*tpl::evaluateField(t, "params.x", "$feature.diameter + chamferExtra", vars, tpl::FieldKind::Length,
                                  eval) == Approx(5.3554));
        CHECK(*tpl::evaluateField(t, "params.x", "floor($feature.depthRatio)", vars, tpl::FieldKind::Unitless, eval) ==
              3);
        CHECK(*tpl::evaluateField(t, "params.x", "0.5 * $tool.diameter", vars, tpl::FieldKind::Length, eval) ==
              Approx(2.5527));
    }
    SECTION("a unit mismatch names the template, the field and the expression")
    {
        auto r =
            tpl::evaluateField(t, "params.spotDiameter", "$feature.diameter + 0.2", vars, tpl::FieldKind::Length, eval);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::InvalidArgument);
        CHECK_THAT(r.error().message,
                   ContainsSubstring("Template 'Spot it'") && ContainsSubstring("params.spotDiameter") &&
                       ContainsSubstring("'$feature.diameter + 0.2'") && ContainsSubstring("unit mismatch"));
        auto plain = tpl::evaluateField(t, "params.dwell", "$feature.diameter", vars, tpl::FieldKind::Unitless, eval);
        REQUIRE_FALSE(plain);
        CHECK_THAT(plain.error().message, ContainsSubstring("expected a plain number"));
    }
    SECTION("unknown, missing and text variables are named")
    {
        auto unknown = tpl::evaluateField(t, "params.peck", "0.5 * $tool.diametr", vars, tpl::FieldKind::Length, eval);
        REQUIRE_FALSE(unknown);
        CHECK_THAT(unknown.error().message,
                   ContainsSubstring("params.peck") && ContainsSubstring("there is no variable tool.diametr"));
        auto missing =
            tpl::evaluateField(t, "params.x", "$feature.counterboreDiameter", vars, tpl::FieldKind::Length, eval);
        REQUIRE_FALSE(missing);
        CHECK_THAT(missing.error().message,
                   ContainsSubstring("feature.counterboreDiameter is not known for this hole"));
        auto text = tpl::evaluateField(t, "params.x", "$feature.threadSpec * 2", vars, tpl::FieldKind::Length, eval);
        REQUIRE_FALSE(text);
        CHECK_THAT(text.error().message, ContainsSubstring("feature.threadSpec is a text"));
        auto dollar = tpl::substitute("$width + 1 mm", vars);
        REQUIRE_FALSE(dollar);
        CHECK_THAT(dollar.error().message, ContainsSubstring("'$width' is not a variable"));
        // A tool variable before the tool is chosen (a tool query).
        auto early = tpl::substitute("$tool.diameter", tpl::featureVars(throughHole(6, 10)));
        REQUIRE_FALSE(early);
        CHECK_THAT(early.error().message, ContainsSubstring("the tool is not chosen yet"));
    }
}

TEST_CASE("a template's rules are read, refused with the key named, and written back", "[cam][templates]")
{
    lib::OpTemplate t = makeTemplate(
        "Peck", "hole.plain",
        R"({"type":"drill","prefer":{"diameter":"$feature.diameter"},"id":"0a6183fa-f4f6-4315-b5ff-b39742bcaee8"})",
        "{}",
        R"({"set":"Drilled holes","sequence":20,"when":{"feature.depthRatio":{"above":3,"max":5},"$feature.through":true},)"
        R"("feeds":{"mode":"fixed","rpm":"1000","feedCut":250,"coolant":"mist"},"heights":{"clearance":"0.6 in"},"other":1})");
    auto r = tpl::rulesOf(t);
    REQUIRE(r);
    CHECK(r->set == "Drilled holes");
    CHECK(r->sequence == 20);
    REQUIRE(r->when.size() == 2); // in key order: "$feature.through" first
    CHECK(r->when[0].variable == "feature.through");
    CHECK(*r->when[0].equals == 1);
    CHECK(r->when[1].variable == "feature.depthRatio");
    CHECK(*r->when[1].above == 3);
    CHECK(*r->when[1].max == 5);
    CHECK_FALSE(r->feeds.calculated);
    CHECK(r->feeds.fixed.at("rpm") == "\"1000\"");
    CHECK(*r->feeds.coolant == cam::CuttingData::Coolant::Mist);
    CHECK(r->heights.at("clearance") == "\"0.6 in\"");
    CHECK(r->toolQueryJson == R"({"type":"drill"})");
    CHECK(r->prefer.at("diameter") == "\"$feature.diameter\"");
    REQUIRE(r->exactTool);
    // Written back (with the template's other unknown keys kept) and read again: the same rules.
    lib::OpTemplate again = t;
    again.extraJson = tpl::extraJsonOf(*r, t.extraJson);
    CHECK_THAT(again.extraJson, ContainsSubstring("\"other\":1"));
    auto r2 = tpl::rulesOf(again);
    REQUIRE(r2);
    CHECK(r2->set == r->set);
    CHECK(r2->when.size() == 2);
    CHECK(r2->feeds.fixed == r->feeds.fixed);
    CHECK(r2->heights == r->heights);

    for (const auto& [extra, key] : {std::pair<std::string, std::string>{R"({"sequence":1.5})", "sequence"},
                                     {R"({"when":{"width":2}})", "when.width"},
                                     {R"({"when":{"feature.depth":{"atLeast":2}}})", "when.feature.depth.atLeast"},
                                     {R"({"feeds":{"mode":"fast"}})", "feeds.mode"},
                                     {R"({"feeds":{"mode":"fixed","rpm":100}})", "feeds"},
                                     {R"({"heights":{"safe":1}})", "heights.safe"}}) {
        lib::OpTemplate bad = t;
        bad.extraJson = extra;
        auto refused = tpl::rulesOf(bad);
        INFO(extra);
        REQUIRE_FALSE(refused);
        CHECK(refused.error().code == ErrorCode::FormatError);
        CHECK_THAT(refused.error().message, ContainsSubstring("Template 'Peck': " + key));
    }
}

TEST_CASE("holes have kinds and fall into groups of alike holes", "[cam][templates]")
{
    const cam::RecognizedHole m6a = tapped("M6x1", 1, 6, 5, 15, 10, 10);
    const cam::RecognizedHole m6b = tapped("M6x1", 1, 6, 5, 15, 30, 10);
    const cam::RecognizedHole m8 = tapped("M8x1.25", 1.25, 8, 6.8, 15, 50, 10);
    const cam::RecognizedHole h66a = throughHole(6.6, 15, 10, 30);
    cam::RecognizedHole h66b = throughHole(6.6000000001, 15, 30, 30); // kernel noise does not split a group
    const cam::RecognizedHole h8 = throughHole(8, 15, 50, 30);
    CHECK(tpl::featureKindsOf(m6a) == std::vector<std::string>{"hole", "hole.tapped", "hole.through"});
    CHECK(tpl::kindMatches("hole.tapped", m6a));
    CHECK_FALSE(tpl::kindMatches("hole.plain", m6a));
    CHECK(tpl::kindMatches("", m6a));
    CHECK(tpl::kindMatches("hole.plain", h66a));
    const std::vector<cam::RecognizedHole> holes = {m6a, h66a, m6b, h66b, m8, h8};
    const auto groups = tpl::groupHoles(holes);
    REQUIRE(groups.size() == 4);
    CHECK(groups[0].kind == "hole.tapped");
    CHECK(groups[0].threadSpec == "M6x1");
    CHECK(groups[0].holes == std::vector<std::size_t>{0, 2});
    CHECK(groups[0].label == "2 x M6x1 tapped, through");
    CHECK(groups[1].holes == std::vector<std::size_t>{1, 3});
    CHECK(groups[1].label == "2 x 6.6 mm, through");
    CHECK(groups[2].threadSpec == "M8x1.25");
    CHECK(groups[3].diameter == 8);
    CHECK(tpl::describeHole(m6b) == "M6x1 at X 30 Y 10");
}

TEST_CASE("the tapped holes set plans spot, tap drill and tap with the right tools in order", "[cam][templates]")
{
    Fixture f;
    const auto tappedSet = f.set("Tapped holes");
    REQUIRE(tappedSet.size() == 5); // spot, three peck alternatives, tap
    // Four 1/4-20 UNC holes through 19.05 mm (drilled #7, 5.1054 mm: depth ratio 3.73) and two M6x1 holes through
    // 10 mm (drilled 5 mm: depth ratio 2).
    std::vector<tpl::PlanHole> holes;
    for (int i = 0; i < 4; ++i) {
        holes.push_back({tapped("1/4-20 UNC", 1.27, 6.35, 5.1054, 19.05, 10.0 * i, 0), {}});
    }
    holes.push_back({tapped("M6x1", 1, 6, 5, 10, 0, 50), {}});
    holes.push_back({tapped("M6x1", 1, 6, 5, 10, 20, 50), {}});
    auto plan = tpl::planTemplates(tappedSet, holes, f.ctx);
    REQUIRE(plan);
    CHECK(plan->missing.empty());
    CHECK(plan->untouched.empty());
    std::vector<std::string> summary;
    for (const tpl::PlannedOperation& op : plan->operations) {
        summary.push_back(op.op.name + " | " + op.tool.name + " | " + paramText(op.op, "cycle") + " | " +
                          std::to_string(op.holes.size()));
        CHECK(op.op.templateId == tappedSet[op.templateIndex].id.toString());
        CHECK(op.op.type == "drill");
        CHECK(op.op.calculated.has_value()); // feeds calculated for 6061-T6
        CHECK(op.op.cutting.spindleRpm > 0);
        CHECK(op.op.cutting.coolant == cam::CuttingData::Coolant::Flood);
    }
    for (const std::string& s : summary) {
        UNSCOPED_INFO(s);
    }
    REQUIRE(plan->operations.size() == 6);
    // Spot: a 1/4 in spot drill reaches both majors (6.35 and 6 mm), so one spot operation; one per thread otherwise
    // differs in its spot diameter.
    CHECK(summary[0] == "Spot drill (1/4-20 UNC) | 1/4 in 90 deg carbide spot drill | \"spot\" | 4");
    CHECK(summary[1] == "Spot drill (M6x1) | 1/4 in 90 deg carbide spot drill | \"spot\" | 2");
    // Tap drills: #7 for 1/4-20 (chip break, depth ratio 3.73 is between 3 and 5 in aluminium), 5 mm for M6 (one feed).
    CHECK(summary[2] == "Tap drill | 5 mm HSS-Co jobber drill | \"drill\" | 2");
    CHECK(summary[3] == "Tap drill, chip break | #7 (0.201 in) HSS-Co jobber drill | \"chipBreak\" | 4");
    // Taps by thread.
    CHECK(summary[4] == "Tap (1/4-20 UNC) | 1/4-20 UNC HSS-E spiral point tap | \"tap\" | 4");
    CHECK(summary[5] == "Tap (M6x1) | M6x1 HSS-E spiral point tap | \"tap\" | 2");
    // The chip-break peck is 1 x the drill's diameter in aluminium.
    CHECK(paramText(plan->operations[3].op, "peck") == "5.1054");
    CHECK(paramText(plan->operations[0].op, "spotDiameter") == "6.35");
    // The plan's operations come in sequence order: spot, drill, tap.
    CHECK(plan->operations[5].op.name.rfind("Tap", 0) == 0);
}

TEST_CASE("a missing tool is listed with its rule and holes, and nothing else fails", "[cam][templates]")
{
    Fixture f;
    const auto tappedSet = f.set("Tapped holes");
    // A thread the library has no tap for (M20x2.5), drilled 17.5 mm (no such drill either).
    std::vector<tpl::PlanHole> holes = {{tapped("M20x2.5", 2.5, 20, 17.5, 30, 5, 5), {}},
                                        {tapped("M20x2.5", 2.5, 20, 17.5, 30, 45, 5), {}}};
    auto plan = tpl::planTemplates(tappedSet, holes, f.ctx);
    REQUIRE(plan);
    REQUIRE(plan->missing.size() == 3); // spot drill (20 mm), tap drill, tap
    const std::string message = tpl::missingToolsMessage(*plan, tappedSet, holes);
    INFO(message);
    CHECK_THAT(message, ContainsSubstring("No tool in the libraries for 'Spot drill'"));
    CHECK_THAT(message,
               ContainsSubstring("'Tap' (tap, thread M20x2.5) for 2 holes (M20x2.5 at X 5 Y 5, M20x2.5 at X 45 Y 5)"));
    CHECK_THAT(message, ContainsSubstring("'Tap drill' (drill, diameter 17.47 to 17.53 mm"));
    CHECK_THAT(message, ContainsSubstring("'Spot drill' (spotDrill, diameter at least 20 mm"));
}

TEST_CASE("peck alternatives follow the depth ratio and drilled holes keep to plain holes", "[cam][templates]")
{
    Fixture f;
    const auto drilled = f.set("Drilled holes");
    REQUIRE(drilled.size() == 4);
    // 6 mm holes 12, 24 and 48 deep (ratios 2, 4, 8) and a tapped hole the plain-hole set leaves alone.
    std::vector<tpl::PlanHole> holes = {{throughHole(6, 12, 0, 0), {}},
                                        {throughHole(6, 24, 20, 0), {}},
                                        {throughHole(6, 48, 40, 0), {}},
                                        {tapped("M6x1", 1, 6, 5, 10, 60, 0), {}}};
    auto plan = tpl::planTemplates(drilled, holes, f.ctx);
    REQUIRE(plan);
    CHECK(plan->untouched == std::vector<std::size_t>{3});
    std::map<std::string, std::string> cycles;
    for (const auto& op : plan->operations) {
        cycles[op.op.name] = paramText(op.op, "cycle") + " " + paramText(op.op, "peck");
        if (op.op.name.rfind("Drill", 0) == 0) {
            CHECK(op.tool.name == "6 mm HSS-Co jobber drill");
        }
    }
    CHECK(cycles.at("Drill") == "\"drill\" ");
    CHECK(cycles.at("Drill, chip break") == "\"chipBreak\" 6");
    CHECK(cycles.at("Drill, peck") == "\"peck\" 3");
}

TEST_CASE("prefer, exact tools, fixed feeds and heights rules", "[cam][templates]")
{
    Fixture f;
    const std::vector<tpl::PlanHole> holes = {{throughHole(6.6, 12), {}}};
    SECTION("the nearest diameter wins; heights and fixed feeds are evaluated")
    {
        const lib::OpTemplate t = makeTemplate(
            "Clearance", "hole.plain",
            R"({"type":"drill","diameter":{"min":"$feature.diameter - 1 mm","max":"$feature.diameter + 1 mm"},"prefer":{"diameter":"$feature.diameter"}})",
            R"({"cycle":"peck","peck":"$feature.depth / 4"})",
            R"({"feeds":{"mode":"fixed","rpm":"2000","feedCut":"0.05 * 2000","coolant":"mist","clockwise":true},)"
            R"("heights":{"clearance":"1 in","retract":"$feature.depth / 4"}})");
        auto plan = tpl::planTemplates(std::vector<lib::OpTemplate>{t}, holes, f.ctx);
        REQUIRE(plan);
        REQUIRE(plan->operations.size() == 1);
        const auto& op = plan->operations.front();
        CHECK(op.tool.name == "6.6 mm HSS-Co jobber drill"); // 6.6 exactly, not 6.8 or 6.528 (F)
        CHECK(op.op.cutting.spindleRpm == 2000);
        CHECK(op.op.cutting.feedCut == Approx(100));
        CHECK(op.op.cutting.coolant == cam::CuttingData::Coolant::Mist);
        CHECK_FALSE(op.op.calculated.has_value());
        CHECK(op.op.heights.clearance == Approx(25.4));
        CHECK(op.op.heights.retract == Approx(3));
        CHECK(op.op.heights.feedPlane == 2); // the default
        CHECK(paramText(op.op, "peck") == "3");
    }
    SECTION("this exact tool")
    {
        const lib::Tool* six = nullptr;
        for (const lib::Tool& t : f.stack.tools()) {
            if (t.name == "6 mm HSS-Co jobber drill") {
                six = f.stack.tool(t.id);
            }
        }
        REQUIRE(six != nullptr);
        const lib::OpTemplate t = makeTemplate(
            "Exactly", "hole", R"({"type":"drill","id":")" + six->id.value.toString() + "\"}", R"({"cycle":"drill"})");
        auto plan = tpl::planTemplates(std::vector<lib::OpTemplate>{t}, holes, f.ctx);
        REQUIRE(plan);
        REQUIRE(plan->operations.size() == 1);
        CHECK(plan->operations.front().tool.id == six->id);
    }
    SECTION("errors name the template, the field and the expression")
    {
        const lib::OpTemplate badParam = makeTemplate("Bad", "hole", R"({"type":"drill","diameter":6.6})",
                                                      R"({"cycle":"peck","peck":"$feature.depth + 2"})");
        auto plan = tpl::planTemplates(std::vector<lib::OpTemplate>{badParam}, holes, f.ctx);
        REQUIRE_FALSE(plan);
        CHECK_THAT(plan.error().message, ContainsSubstring("Template 'Bad', params.peck: '$feature.depth + 2'") &&
                                             ContainsSubstring("unit mismatch"));
        const lib::OpTemplate badKey =
            makeTemplate("Typo", "hole", R"({"type":"drill","diameter":6.6})", R"({"cycel":"drill"})");
        plan = tpl::planTemplates(std::vector<lib::OpTemplate>{badKey}, holes, f.ctx);
        REQUIRE_FALSE(plan);
        CHECK_THAT(plan.error().message, ContainsSubstring("Template 'Typo', params.cycel"));
        const lib::OpTemplate badValue =
            makeTemplate("Range", "hole", R"({"type":"drill","diameter":6.6})", R"({"cycle":"peck","peck":"-1 mm"})");
        plan = tpl::planTemplates(std::vector<lib::OpTemplate>{badValue}, holes, f.ctx);
        REQUIRE_FALSE(plan);
        CHECK_THAT(plan.error().message, ContainsSubstring("Template 'Range', params"));
        const lib::OpTemplate badQuery =
            makeTemplate("Query", "hole", R"({"type":"drill","diameter":"$tool.diameter"})", R"({"cycle":"drill"})");
        plan = tpl::planTemplates(std::vector<lib::OpTemplate>{badQuery}, holes, f.ctx);
        REQUIRE_FALSE(plan);
        CHECK_THAT(plan.error().message,
                   ContainsSubstring("Template 'Query', toolQuery.diameter") && ContainsSubstring("not chosen yet"));
        lib::OpTemplate badType = makeTemplate("Mill", "hole", R"({"type":"drill"})", "{}");
        badType.opType = "pocket9";
        plan = tpl::planTemplates(std::vector<lib::OpTemplate>{badType}, holes, f.ctx);
        REQUIRE_FALSE(plan);
        CHECK_THAT(plan.error().message, ContainsSubstring("'pocket9' is not available"));
    }
    SECTION("calculated feeds need a material")
    {
        f.ctx.material = nullptr;
        const lib::OpTemplate t =
            makeTemplate("Calc", "hole", R"({"type":"drill","diameter":6.6})", R"({"cycle":"drill"})");
        auto plan = tpl::planTemplates(std::vector<lib::OpTemplate>{t}, holes, f.ctx);
        REQUIRE_FALSE(plan);
        CHECK_THAT(plan.error().message, ContainsSubstring("setup has no stock material"));
    }
}

TEST_CASE(
    "the shipped starter templates: three sets for each of the eight materials, sourced and in the library's form",
    "[cam][templates][shipped]")
{
    auto library = lib::ILibrary::open(kLibrary);
    REQUIRE(library);
    for (const auto& issue : (*library)->issues()) {
        FAIL_CHECK(issue.file << ": " << issue.message);
    }
    const auto materials = (*library)->materials();
    REQUIRE(materials.size() == 8);
    const auto templates = (*library)->templates();
    std::map<std::pair<MaterialId, std::string>, int> perSet;
    for (const lib::OpTemplate& t : templates) {
        INFO(t.name);
        auto rules = tpl::rulesOf(t);
        REQUIRE(rules);
        CHECK(t.opType == "drill"); // only operation types that exist today
        REQUIRE(t.material.has_value());
        CHECK(rules->feeds.calculated);
        CHECK(t.comment.find("not copied from any table") != std::string::npos);
        ++perSet[{*t.material, rules->set}];
        // In the form ILibrary::save writes, so saving it again changes no byte.
        const std::string file = kLibrary + "/templates/" + t.id.toString() + ".optemplate.json";
        const std::string text = readFile(file);
        auto again = lib::templateFromJson(text);
        REQUIRE(again);
        CHECK(lib::toJson(*again) == lib::toJson(t));
    }
    for (const lib::Material& m : materials) {
        INFO(m.name);
        CHECK(perSet[{m.id, "Tapped holes"}] == 5);
        CHECK(perSet[{m.id, "Drilled holes"}] == 4);
        CHECK(perSet[{m.id, "Reamed holes"}] == 5);
    }
    CHECK(templates.size() == 8 * 14);
}

TEST_CASE("every starter set plans common metric and inch holes with the shipped tools in every material",
          "[cam][templates][shipped]")
{
    Fixture f;
    // Every thread the library has a tap for (metric and inch), drilled at its tap drill as the hole feature models it
    // (resources/standards/threads.json), 12 mm deep; plain and reamed holes the library has drills and reamers for.
    std::vector<std::tuple<std::string, double, double, double>> threads;
    {
        auto table = cam::json::parse(readFile(std::string(RAY_SOURCE_DIR) + "/resources/standards/threads.json"));
        REQUIRE(table);
        std::map<std::string, const cam::json::Value*> sizes;
        for (const auto& series : table->find("series")->asArray()) {
            for (const auto& size : series.find("sizes")->asArray()) {
                sizes[size.find("designation")->asString()] = &size;
            }
        }
        for (const lib::Tool& t : f.stack.tools()) {
            if (t.type == lib::ToolType::Tap) {
                const cam::json::Value* size = sizes.at(t.threadSpec);
                threads.emplace_back(t.threadSpec, size->find("pitch")->asNumber(),
                                     size->find("majorDiameter")->asNumber(),
                                     size->find("tapDrill")->find("diameter")->asNumber());
            }
        }
        REQUIRE(threads.size() >= 15);
    }
    for (const lib::Material& m : f.stack.materials()) {
        INFO(m.name);
        f.ctx.material = &m;
        std::vector<lib::OpTemplate> tappedSet, drilledSet, reamedSet;
        for (const lib::OpTemplate& t : f.stack.templatesFor("", m.id, MachineId{})) {
            const auto r = tpl::rulesOf(t);
            (r->set == "Tapped holes" ? tappedSet : r->set == "Drilled holes" ? drilledSet : reamedSet).push_back(t);
        }
        std::vector<tpl::PlanHole> holes;
        double x = 0;
        for (const auto& [spec, pitch, major, drill] : threads) {
            holes.push_back({tapped(spec, pitch, major, drill, 12, x += 20, 0), {}});
        }
        auto plan = tpl::planTemplates(tappedSet, holes, f.ctx);
        REQUIRE(plan);
        INFO(tpl::missingToolsMessage(*plan, tappedSet, holes));
        CHECK(plan->missing.empty());
        int taps = 0;
        for (const auto& op : plan->operations) {
            taps += op.tool.type == lib::ToolType::Tap ? static_cast<int>(op.holes.size()) : 0;
        }
        CHECK(taps == static_cast<int>(threads.size()));
        const std::vector<tpl::PlanHole> plain = {{throughHole(6.6, 12, 0, 0), {}},
                                                  {throughHole(6.7564, 19.05, 20, 0), {}},
                                                  {throughHole(10, 30, 40, 0), {}}};
        plan = tpl::planTemplates(drilledSet, plain, f.ctx);
        REQUIRE(plan);
        CHECK(plan->missing.empty());
        const std::vector<tpl::PlanHole> reamed = {
            {throughHole(6, 12, 0, 0), {}}, {throughHole(10, 12, 20, 0), {}}, {throughHole(6.35, 12, 40, 0), {}}};
        plan = tpl::planTemplates(reamedSet, reamed, f.ctx);
        REQUIRE(plan);
        INFO(tpl::missingToolsMessage(*plan, reamedSet, reamed));
        CHECK(plan->missing.empty());
        std::set<std::string> reamers;
        for (const auto& op : plan->operations) {
            if (op.tool.type == lib::ToolType::Reamer) {
                reamers.insert(op.tool.name);
            }
        }
        CHECK(reamers.size() == 3);
    }
}
