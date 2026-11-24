// The fixture panel's logic (task T-077, cam/fixtures_logic.h), without a window: the props of the placement from what
// the panel shows, the edit patch, stored values shown again, where a new placement goes in the history, and the
// shipped fixture models found in the library folder.
#include "cam/fixtures_logic.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>

#include <catch2/catch_test_macros.hpp>

using namespace ray::ui::camui;

namespace {

QJsonObject ref(const QString& body)
{
    return QJsonObject{{QStringLiteral("body"), body},
                       {QStringLiteral("kind"), QStringLiteral("face")},
                       {QStringLiteral("names"), QJsonArray{QStringLiteral("00")}}};
}

std::string text(const QJsonObject& o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();
}

const CheckFn kAccept = [](const QString&, bool, QString*) {
    return true;
};

} // namespace

TEST_CASE("the placement's props: units added to plain numbers, zeros left out, the mate needs both faces",
          "[ui][cam][fixtures]")
{
    PlacementInput in;
    in.body = QStringLiteral("b1");
    in.unitLabel = QStringLiteral("in");
    in.x = QStringLiteral("0.5");
    in.y = QStringLiteral("0");
    in.z = QStringLiteral("-3 mm");
    in.angle = QStringLiteral("90");
    PlacementArgs a = placementProps(in, kAccept);
    REQUIRE(a.error.isEmpty());
    CHECK(text(a.props) == R"({"angle":"90 deg","body":"b1","x":"0.5 in","z":"-3 mm"})");
    // A gap without a mate is refused, naming the field.
    in.gap = QStringLiteral("2");
    a = placementProps(in, kAccept);
    CHECK(a.field == QStringLiteral("gap"));
    // One face picked: the next step is named, not a failure of the numbers.
    in.face = ref(QStringLiteral("b1"));
    a = placementProps(in, kAccept);
    CHECK(a.field == QStringLiteral("target"));
    in.target = ref(QStringLiteral("p1"));
    a = placementProps(in, kAccept);
    REQUIRE(a.error.isEmpty());
    CHECK(a.props.value(QStringLiteral("gap")).toString() == QStringLiteral("2 in"));
    CHECK(a.props.value(QStringLiteral("mate")).toObject().value(QStringLiteral("target")).toObject() ==
          ref(QStringLiteral("p1")));
    // The checker's verdict reaches the field.
    const CheckFn refuseAngles = [](const QString& e, bool angle, QString* error) {
        if (angle)
            *error = QStringLiteral("not an angle: ") + e;
        return !angle;
    };
    a = placementProps(in, refuseAngles);
    CHECK(a.field == QStringLiteral("angle"));
    CHECK(a.error.contains(QStringLiteral("90 deg")));
    // No fixture: nothing to do.
    CHECK_FALSE(placementProps(PlacementInput{}, kAccept).error.isEmpty());
    // Tilts (degrees when plain) are for a move without a mate; with one they are refused, naming the field.
    PlacementInput tilted;
    tilted.body = QStringLiteral("b1");
    tilted.tiltX = QStringLiteral("180");
    tilted.tiltY = QStringLiteral("0");
    a = placementProps(tilted, kAccept);
    REQUIRE(a.error.isEmpty());
    CHECK(text(a.props) == R"({"body":"b1","tiltX":"180 deg"})");
    tilted.face = ref(QStringLiteral("b1"));
    tilted.target = ref(QStringLiteral("p1"));
    a = placementProps(tilted, kAccept);
    CHECK(a.field == QStringLiteral("tiltX"));
}

TEST_CASE("the edit patch removes what the placement no longer has; stored values read back", "[ui][cam][fixtures]")
{
    const QJsonObject props{{QStringLiteral("body"), QStringLiteral("b1")},
                            {QStringLiteral("x"), QStringLiteral("1 mm")}};
    const QJsonObject patch = editPatch(props);
    CHECK(patch.value(QStringLiteral("x")).toString() == QStringLiteral("1 mm"));
    for (const char* key : {"mate", "gap", "angle", "tiltX", "tiltY", "y", "z"})
        CHECK(patch.value(QLatin1String(key)).isNull());
    CHECK(valueText(QJsonValue(12.7), false, QStringLiteral("in")) == QStringLiteral("0.5 in"));
    CHECK(valueText(QJsonValue(12.7), false, QStringLiteral("mm")) == QStringLiteral("12.7 mm"));
    CHECK(valueText(QJsonValue(3.14159265358979323846 / 2), true, QStringLiteral("mm")) == QStringLiteral("90 deg"));
    CHECK(valueText(QJsonValue(QStringLiteral("w / 2")), false, QStringLiteral("mm")) == QStringLiteral("w / 2"));
    CHECK(valueText(QJsonValue(0.0), false, QStringLiteral("mm")).isEmpty());
}

TEST_CASE("a new placement goes before the first one mated to the fixture it moves", "[ui][cam][fixtures]")
{
    const std::vector<std::pair<std::string, std::string>> features = {
        {"import", "{}"},
        {"transform_body", R"({"body":"jaw","mate":{"face":{"body":"jaw"},"target":{"body":"vise"}},"gap":"2 in"})"},
        {"transform_body", R"({"body":"par","x":"1 mm"})"},
    };
    CHECK(placementIndex(features, QStringLiteral("vise")) == 1);
    CHECK_FALSE(placementIndex(features, QStringLiteral("par")).has_value());
    CHECK_FALSE(placementIndex(features, QStringLiteral("jaw")).has_value());
}

TEST_CASE("the shipped fixture models are found in the library's fixtures folder", "[ui][cam][fixtures]")
{
    const std::vector<LibraryFixture> found =
        libraryFixtures({{std::string(RAY_SOURCE_DIR) + "/resources/library", "RayCAD built-in library"},
                         {std::string(RAY_SOURCE_DIR) + "/no-such-library", "gone"}});
    REQUIRE(found.size() == 2);
    CHECK(found[0].name == QStringLiteral("Generic parallels 1x6in"));
    CHECK(found[1].name == QStringLiteral("Generic vise 6in"));
    CHECK(found[1].path.endsWith(QStringLiteral("/resources/library/fixtures/generic-vise-6in.step")));
    CHECK(found[1].root == QStringLiteral("RayCAD built-in library"));
}
