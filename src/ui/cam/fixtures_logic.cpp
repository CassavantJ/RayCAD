#include "fixtures_logic.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QLocale>

#include <algorithm>
#include <cmath>

namespace ray::ui::camui {

namespace {

constexpr const char* kTrContext = "ray::ui::FixturesToolModel";
constexpr double kPi = 3.14159265358979323846;

QString trs(const char* text)
{
    return QCoreApplication::translate(kTrContext, text);
}

// The expression stored for a typed value: a plain number gets its unit; anything else is kept as typed.
QString expressionOf(const QString& raw, const QString& unit)
{
    const QString text = raw.trimmed();
    bool ok = false;
    QLocale::c().toDouble(text, &ok);
    return ok ? text + QLatin1Char(' ') + unit : text;
}

bool isZero(const QString& raw)
{
    const QString text = raw.trimmed();
    if (text.isEmpty())
        return true;
    bool ok = false;
    const double v = QLocale::c().toDouble(text, &ok);
    return ok && v == 0.0;
}

} // namespace

PlacementArgs placementProps(const PlacementInput& in, const CheckFn& check)
{
    PlacementArgs out;
    if (in.body.isEmpty()) {
        out.error = trs("Choose the fixture to place");
        return out;
    }
    QJsonObject props{{QStringLiteral("body"), in.body}};
    const bool mate = !in.face.isEmpty() || !in.target.isEmpty();
    if (mate) {
        if (in.face.isEmpty()) {
            out.error = trs("Click the face of the fixture that touches the part");
            out.field = QStringLiteral("face");
            return out;
        }
        if (in.target.isEmpty()) {
            out.error = trs("Click the face of the part (or of another body) it goes against");
            out.field = QStringLiteral("target");
            return out;
        }
        props.insert(QStringLiteral("mate"),
                     QJsonObject{{QStringLiteral("face"), in.face}, {QStringLiteral("target"), in.target}});
    }
    const struct {
        const char* key;
        const QString* text;
        bool angle;
    } fields[] = {{"gap", &in.gap, false},   {"x", &in.x, false},        {"y", &in.y, false},
                  {"z", &in.z, false},       {"angle", &in.angle, true}, {"tiltX", &in.tiltX, true},
                  {"tiltY", &in.tiltY, true}};
    for (const auto& f : fields) {
        if (isZero(*f.text))
            continue;
        if (QLatin1String(f.key) == QLatin1String("gap") && !mate) {
            out.error = trs("A gap needs a mate: pick the two faces first");
            out.field = QStringLiteral("gap");
            return out;
        }
        if (QLatin1String(f.key).startsWith(QLatin1String("tilt")) && mate) {
            out.error =
                trs("A tilt is for a fixture moved by numbers alone: with a mate the faces set its orientation");
            out.field = QLatin1String(f.key);
            return out;
        }
        const QString expression = expressionOf(*f.text, f.angle ? QStringLiteral("deg") : in.unitLabel);
        QString error;
        if (check && !check(expression, f.angle, &error)) {
            out.error = error;
            out.field = QLatin1String(f.key);
            return out;
        }
        props.insert(QLatin1String(f.key), expression);
    }
    out.props = props;
    return out;
}

QJsonObject editPatch(const QJsonObject& props)
{
    QJsonObject patch = props;
    for (const char* key : {"mate", "gap", "angle", "tiltX", "tiltY", "x", "y", "z"}) {
        if (!patch.contains(QLatin1String(key)))
            patch.insert(QLatin1String(key), QJsonValue::Null);
    }
    return patch;
}

QString valueText(const QJsonValue& value, bool angle, const QString& unitLabel)
{
    if (value.isString())
        return value.toString();
    if (!value.isDouble())
        return {};
    const double v = value.toDouble();
    if (v == 0.0)
        return {};
    if (angle)
        return QLocale::c().toString(v * 180.0 / kPi, 'g', 10) + QStringLiteral(" deg");
    if (unitLabel == QLatin1String("in"))
        return QLocale::c().toString(v / 25.4, 'g', 10) + QStringLiteral(" in");
    return QLocale::c().toString(v, 'g', 10) + QStringLiteral(" mm");
}

std::optional<int> placementIndex(const std::vector<std::pair<std::string, std::string>>& features, const QString& body)
{
    for (std::size_t i = 0; i < features.size(); ++i) {
        if (features[i].first != "transform_body")
            continue;
        const QJsonObject props = QJsonDocument::fromJson(QByteArray::fromStdString(features[i].second)).object();
        const QString target = props.value(QStringLiteral("mate"))
                                   .toObject()
                                   .value(QStringLiteral("target"))
                                   .toObject()
                                   .value(QStringLiteral("body"))
                                   .toString();
        if (!target.isEmpty() && target == body)
            return static_cast<int>(i);
    }
    return std::nullopt;
}

std::vector<LibraryFixture> libraryFixtures(const std::vector<std::pair<std::string, std::string>>& roots)
{
    std::vector<LibraryFixture> out;
    for (const auto& [dir, name] : roots) {
        const QDir folder(QDir(QString::fromStdString(dir)).filePath(QStringLiteral("fixtures")));
        if (!folder.exists())
            continue;
        QStringList files = folder.entryList(
            {QStringLiteral("*.step"), QStringLiteral("*.stp"), QStringLiteral("*.STEP"), QStringLiteral("*.STP")},
            QDir::Files, QDir::Name);
        files.removeDuplicates();
        for (const QString& file : files) {
            QString title = QFileInfo(file).completeBaseName();
            title.replace(QLatin1Char('-'), QLatin1Char(' ')).replace(QLatin1Char('_'), QLatin1Char(' '));
            if (!title.isEmpty())
                title[0] = title[0].toUpper();
            out.push_back(LibraryFixture{title, QDir::fromNativeSeparators(folder.absoluteFilePath(file)),
                                         QString::fromStdString(name)});
        }
    }
    return out;
}

} // namespace ray::ui::camui
