// The tool library manager's non-visual part (src/ui/cam/tools_logic.h, task T-046), without a window:
//
//   - the side view of every tool of the starter library is to scale: as wide as the larger of the cutting and shank
//     diameters, as long as the overall length, the cutting part reaching the cutting diameter, the tip at 0, and the
//     shape of its type (a drill's point angle, a ball's radius, a thread's teeth); in a holder, the holder sits at the
//     stick-out with its gauge length and segment diameters
//   - the drawing depends on the geometry fields only: renaming, vendor text, presets and the comment change neither
//     the geometry hash nor the drawing; every length and angle field the editor shows for a type changes both
//   - the filter: words, type, diameter range, material; the order does not depend on the input order (ties by name,
//     then id) and the document's items come first
//   - the editor's values to the item's file format: problems by field in the schema's words (several at once), a text
//     that does not read, units, unknown keys kept, values shown and not edited kept exactly
#include "cam/tools_logic.h"

#include <ray/libraries.h>
#include <ray_api/cam_tools.h>

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <set>
#include <string>
#include <tuple>

using namespace ray;
using namespace ray::ui;

namespace {

// Lengths as the Units object reads them: a number with an optional unit; the bare number in the display unit.
toolsui::Units unitsOf(bool inch)
{
    toolsui::Units u;
    u.inch = inch;
    u.parseLength = [inch](const QString& text) {
        static const QRegularExpression re(QStringLiteral("^\\s*([-+]?[0-9]*\\.?[0-9]+)\\s*(mm|in)?\\s*$"));
        const auto m = re.match(text);
        if (!m.hasMatch())
            return std::nan("");
        const double v = m.captured(1).toDouble();
        const QString unit = m.captured(2);
        const bool inches = unit.isEmpty() ? inch : unit == QLatin1String("in");
        return inches ? v * 25.4 : v;
    };
    return u;
}

std::vector<lib::Tool> starterTools()
{
    auto library = lib::ILibrary::open(RAY_SOURCE_DIR "/resources/library");
    REQUIRE(library);
    auto tools = (*library)->tools();
    REQUIRE(tools.size() >= 50);
    return tools;
}

std::vector<lib::Holder> starterHolders()
{
    auto library = lib::ILibrary::open(RAY_SOURCE_DIR "/resources/library");
    REQUIRE(library);
    return (*library)->holders();
}

double maxAbsX(const std::vector<Vec2>& poly)
{
    double m = 0;
    for (const Vec2& v : poly)
        m = std::max(m, std::abs(v.x));
    return m;
}
double minY(const std::vector<Vec2>& poly)
{
    double m = 1e300;
    for (const Vec2& v : poly)
        m = std::min(m, v.y);
    return m;
}
double maxY(const std::vector<Vec2>& poly)
{
    double m = -1e300;
    for (const Vec2& v : poly)
        m = std::max(m, v.y);
    return m;
}
bool samePolygons(const toolsui::Profile& a, const toolsui::Profile& b)
{
    const auto same = [](const std::vector<Vec2>& x, const std::vector<Vec2>& y) {
        if (x.size() != y.size())
            return false;
        for (std::size_t i = 0; i < x.size(); ++i) {
            if (x[i].x != y[i].x || x[i].y != y[i].y)
                return false;
        }
        return true;
    };
    return same(a.outline, b.outline) && same(a.cutter, b.cutter) && same(a.holder, b.holder);
}

QString problem(const toolsui::Checked& c, const char* key)
{
    const auto it = c.problems.find(QLatin1String(key));
    return it == c.problems.end() ? QString() : it->second;
}

QJsonObject jsonOf(const lib::Tool& t)
{
    return QJsonDocument::fromJson(QByteArray::fromStdString(api::cam_tools::toolJson(t))).object();
}

} // namespace

TEST_CASE("every starter tool's side view is to scale and has its type's shape", "[tools][profile]")
{
    std::set<lib::ToolType> types;
    for (const lib::Tool& t : starterTools()) {
        INFO(t.name);
        types.insert(t.type);
        const toolsui::Profile p = toolsui::toolProfile(t);
        const double r = t.diameter / 2;
        // As wide as the larger diameter, as long as the tool, the tip at 0, symmetric about the axis.
        CHECK(std::abs(p.halfWidth - std::max(t.diameter, t.shankDiameter) / 2) < 1e-9);
        CHECK(std::abs(p.height - t.overallLength) < 1e-9);
        CHECK(std::abs(minY(p.outline)) < 1e-12);
        for (const Vec2& v : p.outline) {
            const bool mirrored = std::any_of(p.outline.begin(), p.outline.end(), [&](const Vec2& w) {
                return std::abs(w.x + v.x) < 1e-12 && std::abs(w.y - v.y) < 1e-12;
            });
            CHECK(mirrored);
        }
        // The cutting part ends at the flute length and reaches the cutting diameter.
        CHECK(std::abs(maxY(p.cutter) - t.fluteLength) < 1e-9);
        CHECK(std::abs(maxAbsX(p.cutter) - r) < 1e-9);
        CHECK(p.holder.empty());
        switch (t.type) {
        case lib::ToolType::Drill:
        case lib::ToolType::SpotDrill:
        case lib::ToolType::ChamferMill: {
            // A point of the tip angle: the full radius where the cone ends.
            const double h = r / std::tan(t.tipAngleDeg * std::numbers::pi / 360);
            const bool cone = std::any_of(p.outline.begin(), p.outline.end(), [&](const Vec2& v) {
                return std::abs(v.x - r) < 1e-9 && std::abs(v.y - h) < 1e-9;
            });
            CHECK(cone);
            break;
        }
        case lib::ToolType::BallEndMill:
            // Every point of the tip below the radius is on the ball.
            for (const Vec2& v : p.outline) {
                if (v.y < r - 1e-9)
                    CHECK(std::abs(std::hypot(v.x, v.y - r) - r) < 1e-9);
            }
            break;
        case lib::ToolType::Tap:
        case lib::ToolType::ThreadMill:
            if (t.threadPitch > 0) {
                // Teeth of the pitch: the major radius every pitch.
                int majors = 0;
                for (const Vec2& v : p.outline)
                    majors += v.x > 0 && v.y <= t.fluteLength && std::abs(v.x - r) < 1e-9 ? 1 : 0;
                CHECK(majors == static_cast<int>(std::floor(t.fluteLength / t.threadPitch + 1e-9)));
            }
            break;
        default:
            break;
        }
    }
    // The starter library has these types (an end mill of each nose shape, the hole tools, a face mill).
    CHECK(types.size() >= 10);
}

TEST_CASE("in a holder the holder sits at the stick-out with its gauge length and diameters", "[tools][profile]")
{
    const auto tools = starterTools();
    const auto holders = starterHolders();
    REQUIRE(!holders.empty());
    for (const lib::Holder& h : holders) {
        INFO(h.name);
        const lib::Tool& t = tools.front();
        const toolsui::Profile p = toolsui::toolProfile(t, &h, 20);
        CHECK(p.stickOut == 20);
        CHECK(std::abs(minY(p.holder) - 20) < 1e-9);
        double total = 0, widest = 0;
        for (const lib::HolderSegment& s : h.segments) {
            total += s.height;
            widest = std::max({widest, s.lowerDiameter / 2, s.upperDiameter / 2});
        }
        CHECK(std::abs(maxY(p.holder) - (20 + std::max(h.gaugeLength, total))) < 1e-9);
        CHECK(std::abs(maxAbsX(p.holder) - widest) < 1e-9);
        CHECK(std::abs(p.halfWidth - std::max({widest, t.diameter / 2, t.shankDiameter / 2})) < 1e-9);
        // The default stick-out is the shoulder length, never past the tool.
        const toolsui::Profile d = toolsui::toolProfile(t, &h);
        CHECK(d.stickOut == std::min(t.shoulderLength, t.overallLength));
        // A holder alone: its face at 0.
        const toolsui::Profile alone = toolsui::holderProfile(h);
        CHECK(std::abs(minY(alone.holder)) < 1e-9);
        CHECK(alone.outline.empty());
    }
}

TEST_CASE("the drawing changes when and only when the geometry does", "[tools][profile]")
{
    for (const lib::Tool& base : starterTools()) {
        INFO(base.name);
        const toolsui::Profile p0 = toolsui::toolProfile(base);
        // Not geometry: neither the hash nor the drawing changes.
        lib::Tool other = base;
        other.name += " renamed";
        other.vendor = "Someone";
        other.productId = "X-1";
        other.comment = "changed";
        other.threadSpec += "!";
        other.bodyMaterial = "cermet";
        other.defaultToolNumber = 42;
        other.presets.clear();
        CHECK(other.geometryHash() == base.geometryHash());
        CHECK(samePolygons(toolsui::toolProfile(other), p0));
        // Every length and angle the editor shows for the type: both change.
        for (const toolsui::Field& f : toolsui::toolFields(base.type)) {
            const std::string kind = f.kind;
            if (kind != "length" && kind != "angle")
                continue;
            INFO(f.key);
            lib::Tool g = base;
            const std::string key = f.key;
            double* v = key == "diameter"         ? &g.diameter
                        : key == "shankDiameter"  ? &g.shankDiameter
                        : key == "fluteLength"    ? &g.fluteLength
                        : key == "shoulderLength" ? &g.shoulderLength
                        : key == "overallLength"  ? &g.overallLength
                        : key == "cornerRadius"   ? &g.cornerRadius
                        : key == "tipAngle"       ? &g.tipAngleDeg
                        : key == "tipDiameter"    ? &g.tipDiameter
                        : key == "threadPitch"    ? &g.threadPitch
                        : key == "neckDiameter"   ? &g.neckDiameter
                        : key == "neckLength"     ? &g.neckLength
                                                  : nullptr;
            REQUIRE(v != nullptr);
            if (key == "neckLength" && base.neckDiameter <= 0) {
                // A neck length means nothing without a neck: give the tool one, up to above its flutes.
                g.neckDiameter = 0.7 * base.diameter;
                *v = base.fluteLength + 1;
            } else if (key == "tipAngle")
                *v = *v > 0 ? *v - 10 : 100;
            else if (key == "threadPitch" && *v == 0)
                *v = 0.5;
            else if (key == "cornerRadius" && base.type == lib::ToolType::BallEndMill)
                *v = *v * 0.5; // smaller than the ball: a different nose
            else if (key == "tipDiameter")
                *v = *v + base.diameter * 0.2;
            else
                *v = *v * 1.1 + 0.1;
            CHECK(g.geometryHash() != base.geometryHash());
            CHECK_FALSE(samePolygons(toolsui::toolProfile(g), p0));
        }
    }
}

TEST_CASE("the filter is deterministic whatever the input order", "[tools][filter]")
{
    std::vector<toolsui::Entry> entries;
    const char* types[] = {"drill", "flatEndMill", "tap"};
    for (int i = 0; i < 60; ++i) {
        toolsui::Entry e;
        e.id = QStringLiteral("%1").arg(i, 4, 10, QLatin1Char('0'));
        e.name = QStringLiteral("Tool %1").arg(i % 7); // names repeat: ties by id
        e.type = QLatin1String(types[i % 3]);
        e.diameter = 2 + (i % 10);
        e.document = i % 11 == 0;
        e.search = (e.name + QLatin1Char(' ') + e.type).toLower();
        if (i % 4 == 0)
            e.materials = {QStringLiteral("m1")};
        entries.push_back(e);
    }
    const auto ids = [](const std::vector<toolsui::Entry>& all, const std::vector<int>& rows) {
        std::vector<QString> out;
        for (int r : rows)
            out.push_back(all[static_cast<std::size_t>(r)].id);
        return out;
    };
    for (const toolsui::SortKey key : {toolsui::SortKey::Diameter, toolsui::SortKey::Name, toolsui::SortKey::Type}) {
        toolsui::Filter f;
        f.sort = key;
        f.minDiameter = 3;
        f.maxDiameter = 9;
        const auto expected = ids(entries, toolsui::filterEntries(entries, f));
        REQUIRE(!expected.empty());
        // Filtering the list sorted once (what a list does on every key stroke) gives the same.
        const std::vector<int> order = toolsui::sortEntries(entries, key);
        CHECK(ids(entries, toolsui::filterEntries(entries, f, &order)) == expected);
        std::mt19937 rng(7);
        for (int round = 0; round < 5; ++round) {
            auto shuffled = entries;
            std::shuffle(shuffled.begin(), shuffled.end(), rng);
            CHECK(ids(shuffled, toolsui::filterEntries(shuffled, f)) == expected);
        }
        // The document's first; then by the key, ties by name, then id.
        const auto rows = toolsui::filterEntries(entries, f);
        bool library = false;
        for (std::size_t k = 0; k < rows.size(); ++k) {
            const toolsui::Entry& e = entries[static_cast<std::size_t>(rows[k])];
            CHECK(e.diameter >= 3);
            CHECK(e.diameter <= 9);
            if (!e.document)
                library = true;
            else
                CHECK_FALSE(library);
            if (k > 0 && key == toolsui::SortKey::Diameter) {
                const toolsui::Entry& p = entries[static_cast<std::size_t>(rows[k - 1])];
                if (p.document == e.document)
                    CHECK(std::make_tuple(p.diameter, p.name, p.id) <= std::make_tuple(e.diameter, e.name, e.id));
            }
        }
    }
    toolsui::Filter words;
    words.text = QStringLiteral("  TOOL 3   tap ");
    for (int r : toolsui::filterEntries(entries, words)) {
        CHECK(entries[static_cast<std::size_t>(r)].name == QStringLiteral("Tool 3"));
        CHECK(entries[static_cast<std::size_t>(r)].type == QStringLiteral("tap"));
    }
    toolsui::Filter material;
    material.material = QStringLiteral("m1");
    material.type = QStringLiteral("drill");
    const auto m = toolsui::filterEntries(entries, material);
    CHECK(m.size() == 5); // i = 0, 12, 24, 36, 48
    for (int r : m)
        CHECK(entries[static_cast<std::size_t>(r)].type == QStringLiteral("drill"));
    // Several types at once (an operation type's x-toolTypes, the operation panel's picker): any of them.
    toolsui::Filter several;
    several.types = {QStringLiteral("drill"), QStringLiteral("tap")};
    const auto s = toolsui::filterEntries(entries, several);
    CHECK(s.size() == 40); // i % 3 == 0 or 2
    for (int r : s)
        CHECK(entries[static_cast<std::size_t>(r)].type != QStringLiteral("flatEndMill"));
    several.type = QStringLiteral("tap"); // both apply
    for (int r : toolsui::filterEntries(entries, several))
        CHECK(entries[static_cast<std::size_t>(r)].type == QStringLiteral("tap"));
    CHECK(toolsui::filterEntries(entries, several).size() == 20);
}

TEST_CASE("the editor's values are checked against the tool schema, every problem by field", "[tools][fields]")
{
    const lib::Tool base = starterTools().front();
    const QJsonObject json = jsonOf(base);
    const toolsui::Units mm = unitsOf(false);
    toolsui::Values v = toolsui::toolValues(base, mm);
    // As shown: no problem, and the JSON is the tool's exactly.
    toolsui::Checked c = toolsui::checkTool(v, json, mm);
    CHECK(c.ok());
    CHECK(c.json == json);
    CHECK_FALSE(toolsui::edited(v));

    // Several problems at once, each in the schema's words, the lengths with the schema's unit.
    v.fields[QStringLiteral("diameter")] = QStringLiteral("0");
    v.fields[QStringLiteral("overallLength")] = QStringLiteral("3000 mm");
    v.fields[QStringLiteral("name")] = QString();
    v.fields[QStringLiteral("fluteLength")] = QStringLiteral("long");
    REQUIRE(!v.rows.empty());
    v.rows[0].fields[QStringLiteral("chipLoad")] = QStringLiteral("-1 mm");
    v.rows[0].fields[QStringLiteral("maxRadialFraction")] = QStringLiteral("2");
    c = toolsui::checkTool(v, json, mm);
    CHECK(toolsui::edited(v));
    CHECK(problem(c, "diameter") == QStringLiteral("must be greater than 0 mm"));
    CHECK(problem(c, "overallLength") == QStringLiteral("must be at most 2000 mm"));
    CHECK(problem(c, "name") == QStringLiteral("must not be empty"));
    CHECK(problem(c, "fluteLength").startsWith(QStringLiteral("Not a length")));
    CHECK(problem(c, "presets.0.chipLoad") == QStringLiteral("must be at least 0 mm"));
    CHECK(problem(c, "presets.0.maxRadialFraction") == QStringLiteral("must be at most 1"));
    CHECK(c.problems.size() == 6);
    // The valid part keeps what was there: the preview has a tool to draw.
    REQUIRE(!c.valid.isEmpty());
    CHECK(c.valid.value(QStringLiteral("diameter")).toDouble() == base.diameter);

    // Fixed: the edits are in the JSON; the rest (and unknown keys) stays as it was.
    v = toolsui::toolValues(base, mm);
    QJsonObject withExtra = json;
    withExtra[QStringLiteral("x-shop-bin")] = QStringLiteral("B7");
    v.fields[QStringLiteral("diameter")] = QStringLiteral("0.5 in");
    c = toolsui::checkTool(v, withExtra, mm);
    REQUIRE(c.ok());
    CHECK(c.json.value(QStringLiteral("diameter")).toDouble() == 12.7);
    CHECK(c.json.value(QStringLiteral("x-shop-bin")).toString() == QStringLiteral("B7"));
    CHECK(c.json.value(QStringLiteral("overallLength")).toDouble() == base.overallLength);

    // In inches: a value shown and not edited keeps its stored value exactly (no round trip).
    const toolsui::Units in = unitsOf(true);
    v = toolsui::toolValues(base, in);
    CHECK(v.fields[QStringLiteral("diameter")].endsWith(QStringLiteral(" in")));
    CHECK(v.rows[0].fields[QStringLiteral("surfaceSpeed")].endsWith(QStringLiteral(" ft/min")));
    c = toolsui::checkTool(v, json, in);
    CHECK(c.ok());
    CHECK(c.json == json);
    v.rows[0].fields[QStringLiteral("surfaceSpeed")] = QStringLiteral("100 m/min");
    v.fields[QStringLiteral("flutes")] = QStringLiteral("2.5");
    c = toolsui::checkTool(v, json, in);
    CHECK(c.json.value(QStringLiteral("presets"))
              .toArray()
              .at(0)
              .toObject()
              .value(QStringLiteral("surfaceSpeed"))
              .toDouble() == 100);
    CHECK(problem(c, "flutes") == QStringLiteral("Give a whole number"));
}

TEST_CASE("holder values are checked against the holder schema", "[tools][fields]")
{
    const auto holders = starterHolders();
    REQUIRE(!holders.empty());
    const lib::Holder h = holders.front();
    const QJsonObject json = QJsonDocument::fromJson(QByteArray::fromStdString(api::cam_tools::holderJson(h))).object();
    const toolsui::Units mm = unitsOf(false);
    toolsui::Values v = toolsui::holderValues(h, mm);
    CHECK(toolsui::checkHolder(v, json, mm).ok());
    v.rows.push_back(toolsui::newSegmentRow(mm));
    CHECK(toolsui::edited(v));
    v.rows.back().fields[QStringLiteral("height")] = QStringLiteral("0");
    const toolsui::Checked c = toolsui::checkHolder(v, json, mm);
    CHECK(problem(c, QStringLiteral("segments.%1.height").arg(v.rows.size() - 1).toLatin1().constData()) ==
          QStringLiteral("must be greater than 0 mm"));
    // Removing a row is an edit.
    v = toolsui::holderValues(h, mm);
    v.rows.pop_back();
    v.rowsEdited = true;
    CHECK(toolsui::edited(v));
}

TEST_CASE("schema messages name their field", "[tools][fields]")
{
    CHECK(toolsui::splitProblem(QStringLiteral("/presets/3/chipLoad: must be at least 0")) ==
          std::pair<QString, QString>(QStringLiteral("presets.3.chipLoad"), QStringLiteral("must be at least 0")));
    CHECK(toolsui::splitProblem(QStringLiteral("/: needs \"name\"")) ==
          std::pair<QString, QString>(QString(), QStringLiteral("needs \"name\"")));
    CHECK(toolsui::splitProblem(QStringLiteral("Something else")).first.isEmpty());
}
