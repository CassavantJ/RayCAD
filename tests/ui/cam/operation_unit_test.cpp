// The non-visual part of the operation panel (task T-045): the generic parameter form (src/ui/cam/operation_form.h)
// over every operation type the app registers and over synthetic schemas that cover each construct of the subset
// (docs/operation_types.md), with the document's own expressions and units; and the panel's logic
// (operation_logic.h): schema annotations, heights, feeds read-outs and the command's arguments. No window.
#include "cam/operation_form.h"
#include "cam/operation_logic.h"

#include <ray_api/cam_operation.h>
#include <ray_api/cam_tools.h>

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <string>

using namespace ray;
using namespace ray::ui;
using namespace ray::ui::camui;
using Catch::Approx;
using Catch::Matchers::ContainsSubstring;

namespace {

QJsonObject object(const char* json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

QVariantMap fieldOf(const SchemaFormModel& form, const QString& key)
{
    QVariantMap out;
    for (const QVariant& v : form.fields()) {
        if (v.toMap().value(QStringLiteral("key")).toString() == key)
            out = v.toMap();
    }
    INFO("field " << key.toStdString());
    CHECK_FALSE(out.isEmpty());
    return out;
}

// A document with a parameter, read through the app's evaluator (the expressions the feature panels take).
ExprFn documentExpr()
{
    auto state = std::make_shared<doc::DocState>();
    doc::Parameter p;
    p.id = ParamId{Uuid::fromString("11111111-1111-4111-8111-111111111111")};
    p.name = "gap";
    p.expression = "2.5 mm";
    p.kind = doc::Parameter::Kind::Length;
    state->parameters.push_back(p);
    std::shared_ptr<const doc::DocState> s = state;
    return [s](const QString& expression, int kind, QString* error) -> std::optional<double> {
        const doc::Parameter::Kind k = kind == 0   ? doc::Parameter::Kind::Length
                                       : kind == 1 ? doc::Parameter::Kind::Angle
                                                   : doc::Parameter::Kind::Unitless;
        auto v = api::cam_operation::evaluate(*s, expression.toStdString(), k);
        if (!v) {
            *error = QString::fromStdString(v.error().message);
            return std::nullopt;
        }
        return *v;
    };
}

// Every construct of the subset, and every one it refuses.
const char* kEverything = R"({
  "type": "object", "title": "Everything", "additionalProperties": false,
  "x-propertyOrder": ["mode", "depth", "angle", "dwell", "passes", "climb", "label", "nested"],
  "properties": {
    "mode": {"title": "Mode", "description": "How", "enum": ["a", "b", "c"], "x-enumTitles": ["Alpha", "Beta", "Gamma"],
             "default": "a"},
    "depth": {"title": "Depth", "description": "How deep", "type": "number", "exclusiveMinimum": 0, "maximum": 50,
              "default": 10, "x-unit": "length", "x-relevantWhen": {"mode": ["b", "c"]}},
    "angle": {"title": "Angle", "type": "number", "minimum": 0, "maximum": 180, "default": 90, "x-unit": "angle"},
    "dwell": {"title": "Dwell", "type": "number", "minimum": 0, "default": 0.5, "x-unit": "time"},
    "passes": {"title": "Passes", "type": "integer", "minimum": 1, "maximum": 9, "default": 2, "x-unit": "count"},
    "climb": {"title": "Climb", "type": "boolean", "default": true},
    "finish": {"title": "Finish allowance", "type": "number", "minimum": 0, "default": 0.1, "x-unit": "length",
               "x-relevantWhen": {"climb": [false], "mode": ["c"]}},
    "label": {"title": "Label", "type": "string", "minLength": 1, "maxLength": 8, "pattern": "^[A-Z]+$",
              "default": "AB"},
    "nested": {"title": "Nested", "type": "object", "default": {}},
    "list": {"title": "List", "type": "array", "default": []},
    "ref": {"$ref": "#/defs/x"},
    "either": {"title": "Either", "oneOf": [{"type": "number"}, {"type": "string"}], "default": 1},
    "maybe": {"title": "Maybe", "type": ["number", "null"], "default": null},
    "speed": {"title": "Speed", "type": "number", "x-unit": "furlongs", "default": 1},
    "pick": {"title": "Pick", "enum": [1, 2], "default": 1},
    "odd": {"title": "Odd titles", "enum": ["x", "y"], "x-enumTitles": ["Only one"], "default": "x"}
  },
  "allOf": [],
  "if": {"properties": {"mode": {"const": "c"}}}
})";

} // namespace

TEST_CASE("the form of every registered operation type, from its schema alone", "[cam][t045][form]")
{
    // The app's own registry (the built-in types, drill among them), as the panel reads it.
    api::AppOptions options;
    options.userDataDir = "";
    options.builtinLibraryDir = "";
    auto app = api::makeHeadlessApp(options);
    REQUIRE(app);
    const auto types = api::cam_operation::operationTypes(*app);
    REQUIRE_FALSE(types.empty());
    bool drill = false;
    for (const api::cam_operation::TypeInfo& t : types) {
        INFO("operation type " << t.name);
        drill = drill || t.name == "drill";
        SchemaFormModel form;
        form.setSchemaJson(QString::fromStdString(t.schemaJson));
        // Nothing it cannot edit: the built-in types keep to the subset.
        CHECK(form.problems().isEmpty());
        const QJsonObject schema = QJsonDocument::fromJson(QByteArray::fromStdString(t.schemaJson)).object();
        const QJsonObject props = schema.value(QStringLiteral("properties")).toObject();
        REQUIRE(form.fieldCount() == props.size());
        std::set<QString> keys;
        for (const FormField& f : form.schema().fields) {
            INFO("parameter " << f.key.toStdString());
            keys.insert(f.key);
            CHECK(f.kind != FormField::Kind::Unsupported);
            CHECK_FALSE(f.title.isEmpty());
            CHECK_FALSE(f.description.isEmpty());
            CHECK_FALSE(f.defaultValue.isUndefined());
            if (f.kind == FormField::Kind::Enum)
                CHECK(f.enumTitles.size() == f.enumValues.size());
        }
        CHECK(keys.size() == static_cast<std::size_t>(props.size()));
        // The defaults are a valid form, and they are exactly the schema's.
        CHECK(form.valid());
        const QJsonObject params = form.params();
        for (auto it = props.begin(); it != props.end(); ++it)
            CHECK(params.value(it.key()) == it.value().toObject().value(QStringLiteral("default")));
        // Shown as stored and back without a change.
        form.load(params);
        CHECK(form.params() == params);
        // The annotations the other sections read.
        const GeometrySpec g = geometrySpec(schema);
        CHECK((g.kind == QLatin1String("hole") || g.kind == QLatin1String("face") || g.kind == QLatin1String("edge") ||
               g.kind == QLatin1String("none")));
        CHECK(g.problem.isEmpty()); // every kind it names is one the panel picks
        CHECK((g.kinds.isEmpty() || g.kinds.front() == g.kind));
        for (const QString& h : heightsUsed(schema)) {
            CHECK(heightKeys().contains(h));
            // Every built-in type says what each height it reads is measured from (docs/operation_types.md).
            INFO("height " << h.toStdString());
            CHECK_FALSE(heightFromText(heightFrom(schema, h), g.kind).isEmpty());
        }
        for (const QString& c : cuttingUsed(schema))
            CHECK(cuttingKeys().contains(c));
        for (const QString& tool : toolTypesFor(schema, params))
            CHECK(api::cam_tools::toolTypeFromName(tool.toStdString()).has_value());
    }
    CHECK(drill);
}

TEST_CASE("drill's form: order, choices, relevance and tools by cycle", "[cam][t045][form]")
{
    api::AppOptions options;
    options.userDataDir = "";
    options.builtinLibraryDir = "";
    auto app = api::makeHeadlessApp(options);
    REQUIRE(app);
    std::string schemaJson;
    for (const auto& t : api::cam_operation::operationTypes(*app)) {
        if (t.name == "drill")
            schemaJson = t.schemaJson;
    }
    REQUIRE_FALSE(schemaJson.empty());
    SchemaFormModel form;
    form.setSchemaJson(QString::fromStdString(schemaJson));
    form.setExprFn(documentExpr());
    CHECK(form.title() == QStringLiteral("Drilling"));
    REQUIRE(form.fieldCount() >= 3);
    CHECK(form.schema().fields[0].key == QStringLiteral("cycle")); // x-propertyOrder
    CHECK(form.schema().fields[1].key == QStringLiteral("depthFrom"));
    CHECK(fieldOf(form, QStringLiteral("cycle")).value(QStringLiteral("choices")).toList().size() == 7);
    // depth matters only when the depth is given as a value; peck only for the peck cycles.
    CHECK_FALSE(form.isRelevant(QStringLiteral("depth")));
    CHECK_FALSE(form.isRelevant(QStringLiteral("peck")));
    form.setValue(QStringLiteral("depthFrom"), QStringLiteral("value"));
    CHECK(form.isRelevant(QStringLiteral("depth")));
    form.setValue(QStringLiteral("cycle"), QStringLiteral("peck"));
    CHECK(form.isRelevant(QStringLiteral("peck")));
    // The tools on offer follow the cycle (x-toolTypes).
    const QJsonObject schema = form.schema().top;
    CHECK(toolTypesFor(schema, form.params()) == QStringList{QStringLiteral("drill")});
    form.setValue(QStringLiteral("cycle"), QStringLiteral("tap"));
    CHECK(toolTypesFor(schema, form.params()) == QStringList{QStringLiteral("tap")});
    form.setValue(QStringLiteral("cycle"), QStringLiteral("spot"));
    CHECK(toolTypesFor(schema, form.params()).contains(QStringLiteral("spotDrill")));
    CHECK(heightsUsed(schema) ==
          QStringList({QStringLiteral("clearance"), QStringLiteral("retract"), QStringLiteral("top")}));
    CHECK(geometrySpec(schema).kind == QStringLiteral("hole"));
    CHECK(geometrySpec(schema).needsOne());
    // A number with the document's expressions: a parameter, a unit, inches.
    form.setValue(QStringLiteral("depthFrom"), QStringLiteral("value"));
    form.setValue(QStringLiteral("depth"), QStringLiteral("gap * 2"));
    CHECK(form.valueOf(QStringLiteral("depth")).toDouble() == Approx(5.0));
    form.setValue(QStringLiteral("depth"), QStringLiteral("0.5 in"));
    CHECK(form.valueOf(QStringLiteral("depth")).toDouble() == Approx(12.7));
    form.setLengthUnit(QStringLiteral("in"));
    form.setValue(QStringLiteral("depth"), QStringLiteral("1"));
    CHECK(form.valueOf(QStringLiteral("depth")).toDouble() == Approx(25.4));
    form.setValue(QStringLiteral("depth"), QStringLiteral("-1"));
    REQUIRE(form.firstError().has_value());
    CHECK(form.firstError()->first == QStringLiteral("depth"));
    CHECK_THAT(form.firstError()->second.toStdString(), ContainsSubstring("Must be more than 0 in"));
    form.setValue(QStringLiteral("depth"), QStringLiteral("nonsense +"));
    CHECK(form.firstError().has_value());
    form.resetField(QStringLiteral("depth"));
    CHECK(form.valueOf(QStringLiteral("depth")).toDouble() == 10.0); // exactly the default, though shown in inches
    CHECK_FALSE(form.firstError().has_value());
}

// After M3: x-geometry may name several kinds; contour2d takes edges and faces, chamfer2d and engrave edges only (their
// generators refuse faces).
TEST_CASE("x-geometry: one kind or several, and what the panel cannot pick", "[cam][form][geometry]")
{
    const auto spec = [](const char* geometry) {
        return geometrySpec(object((std::string(R"({"x-geometry":)") + geometry + "}").c_str()));
    };
    GeometrySpec g = spec(R"({"kind":"edge","count":"oneOrMore"})");
    CHECK(g.kinds == QStringList{QStringLiteral("edge")});
    CHECK(g.kind == QStringLiteral("edge"));
    CHECK_FALSE(g.several());
    CHECK(g.problem.isEmpty());
    g = spec(R"({"kind":["edge","face"],"count":"oneOrMore"})");
    CHECK(g.kinds == QStringList({QStringLiteral("edge"), QStringLiteral("face")}));
    CHECK(g.kind == QStringLiteral("edge"));
    CHECK(g.several());
    CHECK(g.accepts(QStringLiteral("face")));
    CHECK_FALSE(g.accepts(QStringLiteral("hole")));
    CHECK(g.needsOne());
    CHECK(g.problem.isEmpty());
    g = spec(R"({"kind":["face","face","none"]})");
    CHECK(g.kinds == QStringList{QStringLiteral("face")});
    CHECK(g.problem.isEmpty());
    // A hole with another kind: the hole goes, and the panel says so.
    g = spec(R"({"kind":["hole","face"]})");
    CHECK(g.kinds == QStringList{QStringLiteral("face")});
    CHECK_THAT(g.problem.toStdString(), ContainsSubstring("Holes cannot be picked together"));
    g = spec(R"({"kind":["edge","vertex"]})");
    CHECK(g.kinds == QStringList{QStringLiteral("edge")});
    CHECK_THAT(g.problem.toStdString(), ContainsSubstring("'vertex'"));
    g = spec(R"({"kind":"none"})");
    CHECK(g.kinds.isEmpty());
    CHECK(g.kind == QStringLiteral("none"));
    CHECK(geometrySpec(QJsonObject()).kind == QStringLiteral("none"));
    // The reference of a height measured from each pick, for both kinds.
    CHECK(heightFromText(QStringLiteral("eachPick"), QStringLiteral("edge,face")) ==
          QStringLiteral("from the picked edges and faces"));
    CHECK(heightFromText(QStringLiteral("eachPick"), QStringLiteral("edge")) ==
          QStringLiteral("from the picked edges"));

    // The built-in types.
    api::AppOptions options;
    options.userDataDir = "";
    options.builtinLibraryDir = "";
    auto app = api::makeHeadlessApp(options);
    REQUIRE(app);
    std::map<std::string, QStringList> kinds;
    for (const auto& t : api::cam_operation::operationTypes(*app))
        kinds[t.name] = geometrySpec(QJsonDocument::fromJson(QByteArray::fromStdString(t.schemaJson)).object()).kinds;
    CHECK(kinds["contour2d"] == QStringList({QStringLiteral("edge"), QStringLiteral("face")}));
    CHECK(kinds["chamfer2d"] == QStringList{QStringLiteral("edge")});
    CHECK(kinds["engrave"] == QStringList{QStringLiteral("edge")});
    CHECK(kinds["drill"] == QStringList{QStringLiteral("hole")});
    CHECK(kinds["face"].isEmpty());
}

// After M3: a list of points on the setup's XY plane (x-format "pointsXY"), shown and typed in the display unit,
// stored in mm as the text pocket2d always read ("x y; x y"), so documents written before still load.
TEST_CASE("points: the text read and written, mm whatever the display unit", "[cam][form][points]")
{
    QString error;
    // The grammar pocket2d's parsePointList reads.
    auto p = parsePoints(QStringLiteral(" 20 15; 60,15 ;+1.5\t-2;.5 3."), &error);
    REQUIRE(p.has_value());
    REQUIRE(p->size() == 4);
    CHECK((*p)[1].x == QStringLiteral("60"));
    CHECK((*p)[1].y == QStringLiteral("15"));
    CHECK((*p)[2].x == QStringLiteral("1.5")); // the '+' dropped
    CHECK((*p)[2].y == QStringLiteral("-2"));
    CHECK((*p)[3].x == QStringLiteral(".5"));
    CHECK(pointsText(*p) == QStringLiteral("20 15; 60 15; 1.5 -2; .5 3."));
    CHECK(parsePoints(QString(), &error)->empty());
    CHECK(parsePoints(QStringLiteral("  "), &error)->empty());
    CHECK_FALSE(parsePoints(QStringLiteral("20"), &error).has_value());
    CHECK(error == QStringLiteral("Point 1 has no second number"));
    CHECK_FALSE(parsePoints(QStringLiteral("20 15 3"), &error).has_value());
    CHECK(error == QStringLiteral("After point 1 comes something other than ';'"));
    CHECK_FALSE(parsePoints(QStringLiteral("1 2; x 3"), &error).has_value());
    CHECK(error == QStringLiteral("Point 2 does not start with a number"));
    CHECK_FALSE(parsePoints(QStringLiteral("1e3 2"), &error).has_value()); // fixed-point numbers only
    // Tokens: fixed, a nanometre at most, no exponent, no trailing zeros.
    CHECK(mmToken(20) == QStringLiteral("20"));
    CHECK(mmToken(2.54) == QStringLiteral("2.54"));
    CHECK(mmToken(25.4 / 3) == QStringLiteral("8.466667"));
    CHECK(mmToken(-0.0000001) == QStringLiteral("0"));
    CHECK(mmToken(123456.0000004) == QStringLiteral("123456"));
}

TEST_CASE("points: pocket2d's pre-drilled holes in the form, typed, added, removed, in inches", "[cam][form][points]")
{
    api::AppOptions options;
    options.userDataDir = "";
    options.builtinLibraryDir = "";
    auto app = api::makeHeadlessApp(options);
    REQUIRE(app);
    std::string schemaJson;
    for (const auto& t : api::cam_operation::operationTypes(*app)) {
        if (t.name == "pocket2d")
            schemaJson = t.schemaJson;
    }
    REQUIRE_FALSE(schemaJson.empty());
    SchemaFormModel form;
    form.setExprFn(documentExpr());
    form.setSchemaJson(QString::fromStdString(schemaJson));
    const QString key = QStringLiteral("preDrillPoints");
    CHECK(form.pointKeys() == QStringList{key});
    CHECK(fieldOf(form, key).value(QStringLiteral("kind")).toString() == QStringLiteral("points"));
    CHECK(fieldOf(form, key).value(QStringLiteral("points")).toList().isEmpty());
    // A document written before: its text loads, and goes back exactly as it was while it is not edited.
    const QString old = QStringLiteral("20,15;60 15.25");
    form.load(QJsonObject{{key, old}});
    CHECK(form.params().value(key).toString() == old);
    CHECK(form.valid());
    QVariantList points = fieldOf(form, key).value(QStringLiteral("points")).toList();
    REQUIRE(points.size() == 2);
    CHECK(points[1].toMap().value(QStringLiteral("y")).toString() == QStringLiteral("15.25 mm"));
    CHECK(form.pointsOf(key) == std::vector<std::pair<double, double>>{{20, 15}, {60, 15.25}});
    // In inches: shown in inches, stored in mm; the point not edited keeps its text.
    form.setLengthUnit(QStringLiteral("in"));
    points = fieldOf(form, key).value(QStringLiteral("points")).toList();
    CHECK(points[0].toMap().value(QStringLiteral("x")).toString() == QStringLiteral("0.7874015748 in"));
    form.setPointCoordinate(key, 0, 0, QStringLiteral("1"));
    CHECK(form.params().value(key).toString() == QStringLiteral("25.4 15; 60 15.25"));
    // The coordinate keeps the text as typed while it is edited.
    CHECK(fieldOf(form, key).value(QStringLiteral("points")).toList()[0].toMap().value(QStringLiteral("x")) ==
          QStringLiteral("1"));
    // Expressions and units as length fields take them.
    form.setPointCoordinate(key, 1, 1, QStringLiteral("gap * 2"));
    CHECK(form.pointsOf(key)[1].second == Approx(5.0));
    form.setPointCoordinate(key, 1, 1, QStringLiteral("3 mm"));
    CHECK(form.params().value(key).toString() == QStringLiteral("25.4 15; 60 3"));
    // A coordinate that does not read: the form says where, the stored point stays.
    form.setPointCoordinate(key, 1, 0, QStringLiteral("nonsense +"));
    REQUIRE(form.firstError().has_value());
    CHECK(form.firstError()->first == key);
    CHECK_THAT(form.firstError()->second.toStdString(), ContainsSubstring("Point 2, X"));
    CHECK(fieldOf(form, key).value(QStringLiteral("points")).toList()[1].toMap().value(QStringLiteral("xBad")) == true);
    CHECK(form.pointsOf(key)[1].first == 60);
    form.setPointCoordinate(key, 1, 0, QStringLiteral("2.5"));
    CHECK_FALSE(form.firstError().has_value());
    CHECK(form.params().value(key).toString() == QStringLiteral("25.4 15; 63.5 3"));
    // Added (from a click in the view: WCS mm) and removed.
    form.addPoint(key, 10.25, -4);
    CHECK(form.params().value(key).toString() == QStringLiteral("25.4 15; 63.5 3; 10.25 -4"));
    form.setPointCoordinate(key, 2, 1, QStringLiteral("x"));
    form.removePoint(key, 0);
    CHECK(form.pointsOf(key) == std::vector<std::pair<double, double>>{{63.5, 3}, {10.25, -4}});
    // The coordinate being typed moved with its point.
    CHECK_THAT(form.firstError()->second.toStdString(), ContainsSubstring("Point 2, Y"));
    form.removePoint(key, 1);
    CHECK_FALSE(form.firstError().has_value());
    CHECK(form.params().value(key).toString() == QStringLiteral("63.5 3"));
    form.resetField(key);
    CHECK(form.params().value(key).toString().isEmpty());
    // A stored text that is not points: edited as text until it reads, and not valid meanwhile.
    form.load(QJsonObject{{key, QStringLiteral("20")}});
    CHECK(fieldOf(form, key).value(QStringLiteral("pointsRead")) == false);
    CHECK_FALSE(form.valid());
    form.setValue(key, QStringLiteral("20 1"));
    CHECK(form.valid());
    CHECK(form.pointsOf(key) == std::vector<std::pair<double, double>>{{20, 1}});
    // x-format on another text, or a format the form does not know: plain text.
    SchemaFormModel plain;
    plain.setSchemaJson(QStringLiteral(R"({"type":"object","properties":{"t":{"title":"T","description":"d",)"
                                       R"("type":"string","default":"","x-format":"colour"}}})"));
    CHECK(fieldOf(plain, QStringLiteral("t")).value(QStringLiteral("kind")).toString() == QStringLiteral("string"));
    CHECK(plain.pointKeys().isEmpty());
}

// After M3: x-unit "percent" (pocket2d's stepover and helix diameter), stored as the number of percent.
TEST_CASE("percent: shown with %, read with or without it, limits in percent", "[cam][form][percent]")
{
    CHECK(numberText(45, Unit::Percent, QStringLiteral("in")) == QStringLiteral("45 %"));
    CHECK(unitLabel(Unit::Percent, QStringLiteral("mm")) == QStringLiteral("%"));
    QString error;
    const ExprFn expr = documentExpr();
    CHECK(readNumber(QStringLiteral("50"), Unit::Percent, QStringLiteral("in"), expr, &error) == 50.0);
    CHECK(readNumber(QStringLiteral("50%"), Unit::Percent, QStringLiteral("mm"), expr, &error) == 50.0);
    CHECK(readNumber(QStringLiteral(" 12.5 % "), Unit::Percent, QStringLiteral("mm"), expr, &error) == 12.5);
    CHECK(readNumber(QStringLiteral("10 * 4 %"), Unit::Percent, QStringLiteral("mm"), expr, &error) == 40.0);
    CHECK_FALSE(readNumber(QStringLiteral("%"), Unit::Percent, QStringLiteral("mm"), expr, &error).has_value());
    CHECK_FALSE(readNumber(QStringLiteral("5 mm"), Unit::Percent, QStringLiteral("mm"), expr, &error).has_value());

    api::AppOptions options;
    options.userDataDir = "";
    options.builtinLibraryDir = "";
    auto app = api::makeHeadlessApp(options);
    REQUIRE(app);
    std::string schemaJson;
    for (const auto& t : api::cam_operation::operationTypes(*app)) {
        if (t.name == "pocket2d")
            schemaJson = t.schemaJson;
    }
    SchemaFormModel form;
    form.setExprFn(expr);
    form.setSchemaJson(QString::fromStdString(schemaJson));
    form.setLengthUnit(QStringLiteral("in")); // percent does not follow the length unit
    for (const char* key : {"stepover", "helixDiameter"}) {
        const QVariantMap f = fieldOf(form, QLatin1String(key));
        INFO(key);
        CHECK(f.value(QStringLiteral("unit")).toString() == QStringLiteral("%"));
        CHECK_FALSE(f.value(QStringLiteral("title")).toString().contains(QLatin1Char('%'))); // the unit says it
    }
    CHECK(fieldOf(form, QStringLiteral("stepover")).value(QStringLiteral("value")).toString() ==
          QStringLiteral("45 %"));
    CHECK(fieldOf(form, QStringLiteral("stepover")).value(QStringLiteral("title")).toString() ==
          QStringLiteral("Stepover"));
    CHECK(form.params().value(QStringLiteral("stepover")).toDouble() == 45.0); // stored as it was
    form.setValue(QStringLiteral("stepover"), QStringLiteral("50%"));
    CHECK(form.params().value(QStringLiteral("stepover")).toDouble() == 50.0);
    form.setValue(QStringLiteral("stepover"), QStringLiteral("4"));
    REQUIRE(form.firstError().has_value());
    CHECK(form.firstError()->second == QStringLiteral("Must be at least 5 %"));
    form.setValue(QStringLiteral("stepover"), QStringLiteral("96 %"));
    CHECK(form.firstError()->second == QStringLiteral("Must be at most 95 %"));
    form.resetField(QStringLiteral("stepover"));
    CHECK_FALSE(form.firstError().has_value());
    form.load(QJsonObject{{QStringLiteral("helixDiameter"), 120}});
    CHECK(fieldOf(form, QStringLiteral("helixDiameter")).value(QStringLiteral("value")).toString() ==
          QStringLiteral("120 %"));
    CHECK(form.params().value(QStringLiteral("helixDiameter")).toDouble() == 120.0);
}

// After M3: x-multiline (engrave's text): a text area; line breaks are part of the value; the line spacing in percent.
TEST_CASE("multi-line text: engrave's text on several lines", "[cam][form][multiline]")
{
    api::AppOptions options;
    options.userDataDir = "";
    options.builtinLibraryDir = "";
    auto app = api::makeHeadlessApp(options);
    REQUIRE(app);
    std::string schemaJson;
    for (const auto& t : api::cam_operation::operationTypes(*app)) {
        if (t.name == "engrave")
            schemaJson = t.schemaJson;
    }
    SchemaFormModel form;
    form.setSchemaJson(QString::fromStdString(schemaJson));
    const QVariantMap text = fieldOf(form, QStringLiteral("text"));
    CHECK(text.value(QStringLiteral("kind")).toString() == QStringLiteral("string"));
    CHECK(text.value(QStringLiteral("multiline")).toBool());
    CHECK_FALSE(fieldOf(form, QStringLiteral("textHeight")).value(QStringLiteral("multiline")).toBool());
    form.setValue(QStringLiteral("text"), QStringLiteral("RayCAD\nOp 20"));
    CHECK(form.valid());
    CHECK(form.params().value(QStringLiteral("text")).toString() == QStringLiteral("RayCAD\nOp 20"));
    // The length limit counts the whole text, line breaks included.
    form.setValue(QStringLiteral("text"),
                  QString(150, QLatin1Char('A')) + QLatin1Char('\n') + QString(49, QLatin1Char('B')));
    CHECK(form.valid());
    form.setValue(QStringLiteral("text"),
                  QString(150, QLatin1Char('A')) + QLatin1Char('\n') + QString(50, QLatin1Char('B')));
    REQUIRE(form.firstError().has_value());
    CHECK(form.firstError()->first == QStringLiteral("text"));
    // The line spacing: a percentage of the text height, after the character spacing.
    const QVariantMap spacing = fieldOf(form, QStringLiteral("lineSpacing"));
    CHECK(spacing.value(QStringLiteral("value")).toString() == QStringLiteral("160 %"));
    QStringList keys;
    for (const FormField& f : form.schema().fields)
        keys << f.key;
    CHECK(keys.indexOf(QStringLiteral("lineSpacing")) == keys.indexOf(QStringLiteral("textSpacing")) + 1);
    // x-multiline on a text that is a list of points does not apply (the list is its control).
    SchemaFormModel both;
    both.setSchemaJson(QStringLiteral(R"({"type":"object","properties":{"p":{"title":"P","description":"d",)"
                                      R"("type":"string","default":"","x-format":"pointsXY","x-multiline":true}}})"));
    CHECK(fieldOf(both, QStringLiteral("p")).value(QStringLiteral("kind")).toString() == QStringLiteral("points"));
    CHECK_FALSE(fieldOf(both, QStringLiteral("p")).value(QStringLiteral("multiline")).toBool());
}

TEST_CASE("the form covers each construct of the subset and says what it cannot edit", "[cam][t045][form]")
{
    SchemaFormModel form;
    form.setExprFn(documentExpr());
    form.setSchemaJson(QString::fromLatin1(kEverything));
    const auto& fields = form.schema().fields;
    // Order: x-propertyOrder first (names that are not properties ignored), then the rest by key.
    QStringList keys;
    for (const FormField& f : fields)
        keys << f.key;
    CHECK(keys.mid(0, 6) == QStringList({QStringLiteral("mode"), QStringLiteral("depth"), QStringLiteral("angle"),
                                         QStringLiteral("dwell"), QStringLiteral("passes"), QStringLiteral("climb")}));
    CHECK(keys.indexOf(QStringLiteral("label")) == 6);
    CHECK(keys.size() == 16); // every property has a row, editable or not
    // Kinds.
    const auto kind = [&](const char* key) {
        return fieldOf(form, QLatin1String(key)).value(QStringLiteral("kind")).toString();
    };
    CHECK(kind("mode") == QStringLiteral("enum"));
    CHECK(kind("depth") == QStringLiteral("number"));
    CHECK(kind("passes") == QStringLiteral("integer"));
    CHECK(kind("climb") == QStringLiteral("boolean"));
    CHECK(kind("label") == QStringLiteral("string"));
    for (const char* refused : {"nested", "list", "ref", "either", "maybe", "speed", "pick"}) {
        INFO(refused);
        const QVariantMap f = fieldOf(form, QLatin1String(refused));
        CHECK(f.value(QStringLiteral("kind")).toString() == QStringLiteral("unsupported"));
        CHECK_FALSE(f.value(QStringLiteral("reason")).toString().isEmpty());
        CHECK(f.value(QStringLiteral("relevant")).toBool()); // shown: not silently missing
    }
    // Titles of a choice; a list of titles of the wrong length is not used (and the values are shown).
    const QVariantList choices = fieldOf(form, QStringLiteral("mode")).value(QStringLiteral("choices")).toList();
    REQUIRE(choices.size() == 3);
    CHECK(choices[1].toMap().value(QStringLiteral("text")).toString() == QStringLiteral("Beta"));
    CHECK(fieldOf(form, QStringLiteral("odd"))
              .value(QStringLiteral("choices"))
              .toList()[0]
              .toMap()
              .value(QStringLiteral("text")) == QStringLiteral("x"));
    // Top-level constructs it cannot honour are rows too.
    const QStringList problems = form.problems();
    CHECK(std::any_of(problems.begin(), problems.end(),
                      [](const QString& p) { return p.contains(QLatin1String("allOf")); }));
    CHECK(std::any_of(problems.begin(), problems.end(),
                      [](const QString& p) { return p.contains(QLatin1String("'if'")); }));
    // Units: lengths in the display unit and the document's expressions, angles in degrees, times in seconds.
    CHECK(fieldOf(form, QStringLiteral("depth")).value(QStringLiteral("value")).toString() == QStringLiteral("10 mm"));
    CHECK(fieldOf(form, QStringLiteral("angle")).value(QStringLiteral("value")).toString() == QStringLiteral("90 deg"));
    CHECK(fieldOf(form, QStringLiteral("dwell")).value(QStringLiteral("value")).toString() == QStringLiteral("0.5 s"));
    form.setValue(QStringLiteral("angle"), QStringLiteral("0.5 rad"));
    CHECK(form.valueOf(QStringLiteral("angle")).toDouble() == Approx(28.6478897565));
    form.setValue(QStringLiteral("dwell"), QStringLiteral("250 ms"));
    CHECK(form.valueOf(QStringLiteral("dwell")).toDouble() == Approx(0.25));
    form.setValue(QStringLiteral("dwell"), QStringLiteral("1.5 min"));
    CHECK(form.valueOf(QStringLiteral("dwell")).toDouble() == Approx(90.0));
    form.setValue(QStringLiteral("passes"), QStringLiteral("2.5"));
    CHECK(fieldOf(form, QStringLiteral("passes")).value(QStringLiteral("message")).toString() ==
          QStringLiteral("Must be a whole number"));
    form.setValue(QStringLiteral("passes"), QStringLiteral("12"));
    CHECK(fieldOf(form, QStringLiteral("passes")).value(QStringLiteral("message")).toString() ==
          QStringLiteral("Must be at most 9"));
    form.setValue(QStringLiteral("passes"), QStringLiteral("3"));
    CHECK(form.valueOf(QStringLiteral("passes")).toDouble() == 3.0);
    // Strings: length and pattern.
    form.setValue(QStringLiteral("label"), QStringLiteral("abc"));
    CHECK_FALSE(fieldOf(form, QStringLiteral("label")).value(QStringLiteral("message")).toString().isEmpty());
    form.setValue(QStringLiteral("label"), QStringLiteral("XYZ"));
    CHECK(fieldOf(form, QStringLiteral("label")).value(QStringLiteral("message")).toString().isEmpty());
    // Relevance: depth only for b and c; finish only when climb is off AND mode is c; an irrelevant field is not
    // validated and keeps its value.
    CHECK_FALSE(form.isRelevant(QStringLiteral("depth")));
    form.setValue(QStringLiteral("depth"), QStringLiteral("-5"));
    CHECK_FALSE(form.firstError().has_value());
    CHECK(form.params().value(QStringLiteral("depth")).toDouble() == 10.0); // the default stands in, irrelevant
    form.setValue(QStringLiteral("mode"), QStringLiteral("b"));
    CHECK(form.isRelevant(QStringLiteral("depth")));
    REQUIRE(form.firstError().has_value());
    CHECK(form.firstError()->first == QStringLiteral("depth"));
    form.setValue(QStringLiteral("depth"), QStringLiteral("gap"));
    CHECK(form.valueOf(QStringLiteral("depth")).toDouble() == Approx(2.5));
    CHECK_FALSE(form.isRelevant(QStringLiteral("finish")));
    form.setValue(QStringLiteral("climb"), false);
    CHECK_FALSE(form.isRelevant(QStringLiteral("finish")));
    form.setValue(QStringLiteral("mode"), QStringLiteral("c"));
    CHECK(form.isRelevant(QStringLiteral("finish")));
    // Defaults and reset; a changed field says so.
    CHECK_FALSE(fieldOf(form, QStringLiteral("climb")).value(QStringLiteral("isDefault")).toBool());
    form.resetField(QStringLiteral("climb"));
    CHECK(fieldOf(form, QStringLiteral("climb")).value(QStringLiteral("isDefault")).toBool());
    CHECK(fieldOf(form, QStringLiteral("mode")).value(QStringLiteral("description")).toString() ==
          QStringLiteral("How"));
    // What the form cannot edit is kept as loaded; the rest is stored with its value.
    QJsonObject stored = form.params();
    stored.insert(QStringLiteral("nested"), QJsonObject{{QStringLiteral("k"), 1}});
    stored.insert(QStringLiteral("either"), QStringLiteral("text"));
    form.load(stored);
    const QJsonObject again = form.params();
    CHECK(again.value(QStringLiteral("nested")).toObject().value(QStringLiteral("k")).toInt() == 1);
    CHECK(again.value(QStringLiteral("either")).toString() == QStringLiteral("text"));
    CHECK(again.value(QStringLiteral("mode")).toString() == QStringLiteral("c"));
    // An external message (the API's check) shows beside its field until the field changes.
    form.setExternalMessage(QStringLiteral("angle"), QStringLiteral("must be at most 180"));
    CHECK(fieldOf(form, QStringLiteral("angle")).value(QStringLiteral("message")).toString() ==
          QStringLiteral("must be at most 180"));
    form.setValue(QStringLiteral("angle"), QStringLiteral("45"));
    CHECK(fieldOf(form, QStringLiteral("angle")).value(QStringLiteral("message")).toString().isEmpty());
}

TEST_CASE("schemas the form cannot use do not break it", "[cam][t045][form]")
{
    for (const char* text : {"", "not json", "[1, 2]", R"({"type":"array"})", R"({"properties": 5})",
                             R"({"type":"object","properties":{"a":7,"b":{"type":"number"}}})",
                             R"({"type":"object","properties":{"a":{"type":"number","default":1,
                                 "x-relevantWhen":{"nope":["x"],"b":[true]}},"b":{"type":"string","default":""}}})"}) {
        INFO(text);
        SchemaFormModel form;
        form.setSchemaJson(QString::fromLatin1(text));
        (void)form.fields();
        (void)form.params();
        (void)form.valid();
        const bool problem = !form.problems().isEmpty() ||
                             std::any_of(form.schema().fields.begin(), form.schema().fields.end(),
                                         [](const FormField& f) { return f.kind == FormField::Kind::Unsupported; });
        CHECK(problem);
    }
    // A field without a default: editable, nothing to reset to.
    SchemaFormModel form;
    form.setSchemaJson(QStringLiteral(R"({"type":"object","properties":{"passes":{"type":"integer","minimum":1}}})"));
    CHECK(form.problems().isEmpty());
    CHECK_FALSE(fieldOf(form, QStringLiteral("passes")).value(QStringLiteral("hasDefault")).toBool());
    CHECK(form.firstError().has_value()); // empty: "Enter a value"
    form.setValue(QStringLiteral("passes"), QStringLiteral("4"));
    CHECK(form.params().value(QStringLiteral("passes")).toDouble() == 4.0);
}

TEST_CASE("the panel's logic: heights, feeds in display units, records, tools and arguments", "[cam][t045]")
{
    // Unused heights take the nearest used one before them, else after (docs/operation_types.md).
    const auto h = completeHeights(
        {{QStringLiteral("clearance"), 15}, {QStringLiteral("retract"), 2}, {QStringLiteral("top"), 0}});
    CHECK(h.at(QStringLiteral("feedPlane")) == 2);
    CHECK(h.at(QStringLiteral("bottom")) == 0);
    const auto only = completeHeights({{QStringLiteral("top"), 1}, {QStringLiteral("bottom"), -3}});
    CHECK(only.at(QStringLiteral("clearance")) == 1);
    // Out of order only between heights the type reads from one reference (x-heightsUsed): face's bottom, from the
    // part top, may be above its top, from the stock top; two heights from the stock top may not.
    const QJsonObject face = object(
        R"({"x-heightsUsed":{"clearance":"stockTop","retract":"stockTop","feedPlane":"stockTop","top":"stockTop",)"
        R"("bottom":"partTop"},"x-geometry":{"kind":"none"}})");
    CHECK_FALSE(heightsOutOfOrder(face, h).has_value());
    CHECK_FALSE(heightsOutOfOrder(face, {{QStringLiteral("clearance"), 15},
                                         {QStringLiteral("retract"), 5},
                                         {QStringLiteral("feedPlane"), 2},
                                         {QStringLiteral("top"), 0},
                                         {QStringLiteral("bottom"), 0.2}})
                    .has_value());
    const auto bad = heightsOutOfOrder(face, {{QStringLiteral("clearance"), 1}, {QStringLiteral("retract"), 5}});
    REQUIRE(bad.has_value());
    CHECK(bad->first == QStringLiteral("clearance"));
    CHECK(bad->second == QStringLiteral("retract"));
    // Drill: clearance from the highest hole top over retract from each hole's top is checked; a schema that does not
    // say what its heights are measured from has nothing checked here.
    const QJsonObject drill =
        object(R"({"x-heightsUsed":{"clearance":"highestPick","retract":"eachPick","top":"eachPick"}})");
    CHECK(heightsOutOfOrder(drill, {{QStringLiteral("clearance"), 1}, {QStringLiteral("retract"), 5}}).has_value());
    CHECK_FALSE(heightsOutOfOrder(drill, {{QStringLiteral("clearance"), 15},
                                          {QStringLiteral("retract"), 5},
                                          {QStringLiteral("top"), 0},
                                          {QStringLiteral("bottom"), 9}})
                    .has_value());
    CHECK_FALSE(heightsOutOfOrder(object(R"({"x-heightsUsed":["clearance","retract"]})"),
                                  {{QStringLiteral("clearance"), 1}, {QStringLiteral("retract"), 5}})
                    .has_value());
    CHECK(heightsUsed(QJsonObject()) == heightKeys());
    CHECK(heightsUsed(drill) ==
          QStringList({QStringLiteral("clearance"), QStringLiteral("retract"), QStringLiteral("top")}));
    CHECK(heightFrom(face, QStringLiteral("bottom")) == QStringLiteral("partTop"));
    CHECK(heightFrom(object(R"({"x-heightsUsed":["top"]})"), QStringLiteral("top")).isEmpty());
    CHECK(heightFromText(QStringLiteral("stockTop"), QStringLiteral("none")) == QStringLiteral("from stock top"));
    CHECK(heightFromText(QStringLiteral("partTop"), QStringLiteral("none")) == QStringLiteral("from part top"));
    CHECK(heightFromText(QStringLiteral("eachPick"), QStringLiteral("edge")) ==
          QStringLiteral("from the picked edges"));
    CHECK(heightFromText(QStringLiteral("eachPick"), QStringLiteral("hole")) == QStringLiteral("from each hole's top"));
    CHECK(heightFromText(QStringLiteral("highestPick"), QStringLiteral("hole")) ==
          QStringLiteral("from the highest hole top"));
    CHECK(heightFromText(QString(), QStringLiteral("hole")).isEmpty());

    // The type list's order, and the type a new operation starts with.
    const QStringList registered = {
        QStringLiteral("chamfer2d"), QStringLiteral("contour2d"), QStringLiteral("drill"), QStringLiteral("engrave"),
        QStringLiteral("face"),      QStringLiteral("pocket2d"),  QStringLiteral("slot"),  QStringLiteral("threadmill"),
        QStringLiteral("x.zeta"),    QStringLiteral("x.alpha")};
    CHECK(orderedTypeNames(registered) ==
          QStringList({QStringLiteral("drill"), QStringLiteral("face"), QStringLiteral("contour2d"),
                       QStringLiteral("pocket2d"), QStringLiteral("slot"), QStringLiteral("chamfer2d"),
                       QStringLiteral("engrave"), QStringLiteral("threadmill"), QStringLiteral("x.alpha"),
                       QStringLiteral("x.zeta")}));
    CHECK(defaultTypeName(registered, QString()) == QStringLiteral("drill"));
    CHECK(defaultTypeName(registered, QStringLiteral("pocket2d")) == QStringLiteral("pocket2d"));
    CHECK(defaultTypeName(registered, QStringLiteral("gone")) == QStringLiteral("drill"));
    CHECK(defaultTypeName({QStringLiteral("x.zeta"), QStringLiteral("x.alpha")}, QString()) ==
          QStringLiteral("x.alpha"));
    CHECK(defaultTypeName({}, QString()).isEmpty());
    CHECK(cuttingUsed(object(R"({"x-cuttingUsed":["coolant"]})")) ==
          QStringList({QStringLiteral("rpm"), QStringLiteral("feedCut"), QStringLiteral("coolant")}));

    // Feeds: metric and inch read-outs; reading them back.
    CHECK(feedText(320, false) == QStringLiteral("320 mm/min"));
    CHECK(feedText(254, true) == QStringLiteral("10 in/min"));
    CHECK(surfaceSpeedText(304.8, true) == QStringLiteral("1000 sfm"));
    CHECK(surfaceSpeedText(100, false) == QStringLiteral("100 m/min"));
    CHECK(chipLoadText(0.0508, true) == QStringLiteral("0.0020 in/tooth"));
    CHECK(rpmText(1200) == QStringLiteral("1200 rpm"));
    const ExprFn expr = plainExprFn();
    QString error;
    CHECK(*readFeed(QStringLiteral("10"), true, expr, &error) == Approx(254));
    CHECK(*readFeed(QStringLiteral("300 mm/min"), true, expr, &error) == Approx(300));
    CHECK(*readFeed(QStringLiteral("12 ipm"), false, expr, &error) == Approx(304.8));
    CHECK_FALSE(readFeed(QStringLiteral("0"), false, expr, &error).has_value());
    CHECK(*readRpm(QStringLiteral("1500 rpm"), expr, &error) == 1500);
    CHECK_FALSE(readRpm(QString(), expr, &error).has_value());
    // A record matches the values it wrote (the members the type uses).
    cam::CalculatedFeeds r;
    r.spindleRpm = 1000;
    r.feedCut = 200;
    r.feedPlunge = 100;
    r.feedRamp = 150;
    CHECK(matchesRecord(r, 1000, 200, 999, 999, {QStringLiteral("rpm"), QStringLiteral("feedCut")}));
    CHECK_FALSE(matchesRecord(r, 1000, 200, 999, 150, cuttingKeys()));
    CHECK_FALSE(matchesRecord(r, 1001, 200, 100, 150, cuttingKeys()));
    CHECK(limitStateText(cam::LimitState::Applied) == QStringLiteral("Applied"));

    // Arguments: the members the commands take; the library items in the same step; the record when it changed.
    OperationInput in;
    in.id = QStringLiteral("c0000000-0000-4000-8000-000000000001");
    in.setup = QStringLiteral("a0000000-0000-4000-8000-000000000001");
    in.type = QStringLiteral("drill");
    in.name = QStringLiteral("Drill 1");
    in.tool = QStringLiteral("d0000000-0000-4000-8000-000000000001");
    in.rpm = 1000;
    in.feedCut = 200;
    in.feedPlunge = 200;
    in.heights = completeHeights(
        {{QStringLiteral("clearance"), 15}, {QStringLiteral("retract"), 2}, {QStringLiteral("top"), 0}});
    in.params = object(R"({"cycle":"peck"})");
    in.libraryTools.push_back(QJsonObject{{QStringLiteral("id"), in.tool}});
    in.calculated = calculatedJson(r);
    const QJsonObject add = operationArgs(in, true);
    CHECK(add.value(QStringLiteral("type")).toString() == QStringLiteral("drill"));
    CHECK(add.value(QStringLiteral("setup")).toString() == in.setup);
    CHECK(add.value(QStringLiteral("tool")).toObject().value(QStringLiteral("holder")).isNull());
    CHECK(add.value(QStringLiteral("heights")).toObject().size() == 5);
    CHECK(add.value(QStringLiteral("library")).toObject().value(QStringLiteral("tools")).toArray().size() == 1);
    CHECK(add.value(QStringLiteral("calculatedFeeds")).toObject().value(QStringLiteral("rpm")).toDouble() == 1000);
    in.calculated = QJsonValue(QJsonValue::Undefined);
    const QJsonObject edit = operationArgs(in, false);
    CHECK_FALSE(edit.contains(QStringLiteral("type")));
    CHECK_FALSE(edit.contains(QStringLiteral("setup")));
    CHECK_FALSE(edit.contains(QStringLiteral("calculatedFeeds")));
}

// M3 closeout: five library tools added through the panel with the number left empty were all T1 (their own number is
// 0), so the program never changed tools. A new operation's empty number is the document's for that tool, else a free
// one.
TEST_CASE("a new operation's tool number: the document's for the tool, else the tool's own if free, else the next",
          "[cam][operation][ui]")
{
    const auto uuid = [](char c) {
        std::string s = "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx";
        std::replace(s.begin(), s.end(), 'x', c);
        return Uuid::fromString(s);
    };
    const auto tool = [&](char c) {
        return ToolId{uuid(c)};
    };
    cam::CamState cam;
    CHECK(newToolNumber(nullptr, tool('1'), 0) == 1);
    CHECK(newToolNumber(&cam, tool('1'), 0) == 1);
    CHECK(newToolNumber(&cam, tool('1'), 7) == 7);
    cam::Setup setup;
    const auto add = [&](char opChar, char toolChar, int number) {
        cam::Operation op;
        op.id = OperationId{uuid(opChar)};
        op.tool.tool = tool(toolChar);
        op.tool.toolNumber = number;
        cam.operations[op.id] = op;
        setup.operations.push_back(op.id);
        cam.setups = {setup};
    };
    add('a', '1', 1);
    CHECK(newToolNumber(&cam, tool('1'), 0) == 1); // the document's number for the tool
    CHECK(newToolNumber(&cam, tool('2'), 0) == 2); // another tool: the next number
    CHECK(newToolNumber(&cam, tool('2'), 1) == 2); // its own number is taken by another tool
    CHECK(newToolNumber(&cam, tool('2'), 12) == 12);
    add('b', '2', 5);
    add('c', '1', 9); // the first operation in program order decides the tool's number
    CHECK(newToolNumber(&cam, tool('1'), 0) == 1);
    CHECK(newToolNumber(&cam, tool('2'), 0) == 5);
    CHECK(newToolNumber(&cam, tool('3'), 0) == 10);
}
