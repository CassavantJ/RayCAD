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

#include <set>

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
        for (const QString& h : heightsUsed(schema))
            CHECK(heightKeys().contains(h));
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
    CHECK_FALSE(heightsOutOfOrder(h).has_value());
    const auto bad = heightsOutOfOrder({{QStringLiteral("clearance"), 1}, {QStringLiteral("retract"), 5}});
    REQUIRE(bad.has_value());
    CHECK(bad->first == QStringLiteral("clearance"));
    CHECK(heightsUsed(QJsonObject()) == heightKeys());
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
